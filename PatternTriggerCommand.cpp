#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <regex>
#include <direct.h>
#include <set>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <map>
#include <memory>

// Autore: Umberto Meglio
// Supporto alla creazione: Claude di Anthropic

// Definizioni per compatibilità MinGW
#ifndef ERROR_OPERATION_ABORTED
#define ERROR_OPERATION_ABORTED 995L
#endif

#ifndef SERVICE_CONFIG_DESCRIPTION
#define SERVICE_CONFIG_DESCRIPTION 1
#endif

typedef BOOL (WINAPI *CHANGESERVICECONFIG2PROC)(SC_HANDLE hService, DWORD dwInfoLevel, LPVOID lpInfo);

BOOL MyChangeServiceConfig2(SC_HANDLE hService, DWORD dwInfoLevel, LPVOID lpInfo) {
    HMODULE hModule = LoadLibrary("advapi32.dll");
    if (hModule == NULL) return FALSE;
    
    CHANGESERVICECONFIG2PROC changeServiceConfig2Proc = 
        (CHANGESERVICECONFIG2PROC)GetProcAddress(hModule, "ChangeServiceConfig2A");
    
    BOOL result = FALSE;
    if (changeServiceConfig2Proc != NULL) {
        result = changeServiceConfig2Proc(hService, dwInfoLevel, lpInfo);
    }
    
    FreeLibrary(hModule);
    return result;
}

struct SERVICE_DESCRIPTION_STRUCT {
    LPSTR lpDescription;
};

// Configurazione del servizio
#define SERVICE_NAME "PatternTriggerCommand"
#define SERVICE_DISPLAY_NAME "Pattern Trigger Command Service"
#define SERVICE_DESCRIPTION "Monitora cartelle multiple per file che corrispondono a pattern configurati ed esegue comandi associati"

// Percorsi predefiniti
#define DEFAULT_MONITORED_FOLDER "C:\\Monitored"
#define DEFAULT_CONFIG_FILE "C:\\PTC\\config.ini"
#define DEFAULT_LOG_FILE "C:\\PTC\\PatternTriggerCommand.log"
#define DEFAULT_DETAILED_LOG_FILE "C:\\PTC\\PatternTriggerCommand_detailed.log"
#define DEFAULT_PROCESSED_FILES_DB "C:\\PTC\\PatternTriggerCommand_processed.txt"

// Intervalli di tempo
#define FILE_CHECK_INTERVAL 3000
#define MONITORING_RESTART_DELAY 2000
#define BATCH_TIMEOUT 60000
#define CACHE_CLEANUP_INTERVAL 300000
#define SERVICE_SHUTDOWN_TIMEOUT 15000
#define DIR_CHANGES_WAIT_TIMEOUT 30000

// Variabili globali del servizio
SERVICE_STATUS serviceStatus;
SERVICE_STATUS_HANDLE serviceStatusHandle;
HANDLE stopEvent = NULL;

// Configurazione
std::string defaultMonitoredFolder = DEFAULT_MONITORED_FOLDER;
std::string configFile = DEFAULT_CONFIG_FILE;
std::string logFile = DEFAULT_LOG_FILE;
std::string detailedLogFile = DEFAULT_DETAILED_LOG_FILE;
std::string processedFilesDb = DEFAULT_PROCESSED_FILES_DB;
bool detailedLogging = true;

// Struttura per pattern e comandi
struct PatternCommandPair {
    std::string folderPath;
    std::string patternRegex;
    std::string command;
    std::regex compiledRegex;
    std::string patternName;
    
    PatternCommandPair(const std::string& folder, const std::string& pattern, 
                      const std::string& cmd, const std::string& name = "") 
        : folderPath(folder), patternRegex(pattern), command(cmd), 
          compiledRegex(pattern, std::regex_constants::icase), patternName(name) {}
};

std::vector<PatternCommandPair> patternCommandPairs;

// Gestione dei file processati
std::set<std::string> processedFiles;
std::set<std::string> recentlyIgnoredFiles;

// Struttura per monitoraggio cartella
struct FolderMonitor {
    std::string folderPath;
    std::string normalizedPath;
    std::vector<int> patternIndices;
    HANDLE hThread;
    HANDLE hStopEvent;
    DWORD threadId;
    bool active;
    
    FolderMonitor(const std::string& path) : folderPath(path), hThread(NULL), 
        hStopEvent(NULL), threadId(0), active(false) {
        normalizedPath = path;
        // Normalizza il percorso
        std::replace(normalizedPath.begin(), normalizedPath.end(), '/', '\\');
        if (!normalizedPath.empty() && normalizedPath.back() == '\\') {
            normalizedPath.pop_back();
        }
        std::transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(), ::toupper);
    }
    
    ~FolderMonitor() {
        if (hThread) {
            CloseHandle(hThread);
        }
        if (hStopEvent) {
            CloseHandle(hStopEvent);
        }
    }
};

std::map<std::string, std::unique_ptr<FolderMonitor>> folderMonitors;

// ====== DICHIARAZIONI FUNZIONI ======

std::string GetTimestamp();
void WriteToLog(const std::string& message, bool detailed = false);
std::string NormalizeFolderPath(const std::string& path);
bool FileExists(const std::string& filename);
bool DirectoryExists(const std::string& path);
bool CreateDirectoryRecursive(const std::string& path);
bool IsFileInUse(const std::string& filePath);
bool WaitForFileAvailability(const std::string& filePath, int maxWaitTimeMs = 30000);
void LoadProcessedFiles();
void SaveProcessedFiles();
bool IsFileAlreadyProcessed(const std::string& fullFilePath);
void MarkFileAsProcessed(const std::string& fullFilePath);
bool LoadConfiguration();
std::vector<int> FindMatchingPatterns(const std::string& filename, const std::string& folderPath);
bool ExecuteCommand(const std::string& command, const std::string& parameter, const std::string& patternName);
void ScanDirectoryForExistingFiles(const std::string& folderPath, const std::vector<int>& patternIndices);
void ProcessDirectoryNotifications(BYTE* buffer, DWORD bytesTransferred, FolderMonitor* monitor);
DWORD WINAPI FolderMonitorThreadFunction(LPVOID lpParam);
void StartAllFolderMonitors();
void StopAllFolderMonitors();
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);
void WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType);

// ====== IMPLEMENTAZIONE FUNZIONI ======

std::string GetTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    
    std::ostringstream oss;
    oss << std::setfill('0') 
        << st.wYear << "-" 
        << std::setw(2) << st.wMonth << "-" 
        << std::setw(2) << st.wDay << " "
        << std::setw(2) << st.wHour << ":" 
        << std::setw(2) << st.wMinute << ":" 
        << std::setw(2) << st.wSecond << "." 
        << std::setw(3) << st.wMilliseconds;
    
    return oss.str();
}

void WriteToLog(const std::string& message, bool detailed) {
    std::ofstream logFileStream(logFile.c_str(), std::ios::app);
    if (logFileStream.is_open()) {
        logFileStream << GetTimestamp() << " - " << message << std::endl;
        logFileStream.close();
    }
    
    if (detailed && detailedLogging) {
        std::ofstream detailedLogFileStream(detailedLogFile.c_str(), std::ios::app);
        if (detailedLogFileStream.is_open()) {
            detailedLogFileStream << GetTimestamp() << " - " << message << std::endl;
            detailedLogFileStream.close();
        }
    }
}

std::string NormalizeFolderPath(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    
    if (!normalized.empty() && normalized.back() == '\\') {
        normalized.pop_back();
    }
    
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::toupper);
    return normalized;
}

bool FileExists(const std::string& filename) {
    DWORD attrs = GetFileAttributes(filename.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

bool DirectoryExists(const std::string& path) {
    DWORD attrs = GetFileAttributes(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
}

bool CreateDirectoryRecursive(const std::string& path) {
    std::string normalizedPath = path;
    std::replace(normalizedPath.begin(), normalizedPath.end(), '/', '\\');
    
    if (!normalizedPath.empty() && normalizedPath.back() == '\\') {
        normalizedPath.pop_back();
    }
    
    if (DirectoryExists(normalizedPath)) {
        return true;
    }
    
    size_t pos = normalizedPath.find_last_of('\\');
    if (pos != std::string::npos) {
        std::string parentPath = normalizedPath.substr(0, pos);
        if (!CreateDirectoryRecursive(parentPath)) {
            return false;
        }
    }
    
    return CreateDirectory(normalizedPath.c_str(), NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool IsFileInUse(const std::string& filePath) {
    HANDLE hFile = CreateFile(filePath.c_str(), GENERIC_READ, 0, NULL, 
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        return false;
    }
    
    DWORD error = GetLastError();
    return (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION);
}

bool WaitForFileAvailability(const std::string& filePath, int maxWaitTimeMs) {
    int waitTime = 0;
    const int sleepInterval = 500;
    
    WriteToLog("Verifica disponibilità file: " + filePath, true);
    
    while (waitTime < maxWaitTimeMs) {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            WriteToLog("Terminazione richiesta durante attesa file", true);
            return false;
        }
        
        if (!FileExists(filePath)) {
            WriteToLog("File non più presente: " + filePath, true);
            return false;
        }
        
        if (!IsFileInUse(filePath)) {
            WriteToLog("File disponibile dopo " + std::to_string(waitTime) + "ms", true);
            return true;
        }
        
        Sleep(sleepInterval);
        waitTime += sleepInterval;
    }
    
    return false;
}

void LoadProcessedFiles() {
    std::ifstream file(processedFilesDb.c_str());
    std::string line;
    
    processedFiles.clear();
    
    if (file.is_open()) {
        while (std::getline(file, line)) {
            if (!line.empty()) {
                processedFiles.insert(line);
            }
        }
        file.close();
        WriteToLog("Caricati " + std::to_string(processedFiles.size()) + " file dal database");
    } else {
        WriteToLog("Database file processati non trovato, verrà creato");
    }
}

void SaveProcessedFiles() {
    std::ofstream file(processedFilesDb.c_str());
    if (file.is_open()) {
        for (const auto& filename : processedFiles) {
            file << filename << std::endl;
        }
        file.close();
        WriteToLog("Salvati " + std::to_string(processedFiles.size()) + " file nel database");
    } else {
        WriteToLog("ERRORE: Impossibile salvare database file processati");
    }
}

bool IsFileAlreadyProcessed(const std::string& fullFilePath) {
    return processedFiles.find(fullFilePath) != processedFiles.end();
}

void MarkFileAsProcessed(const std::string& fullFilePath) {
    processedFiles.insert(fullFilePath);
    SaveProcessedFiles();
    WriteToLog("File marcato come processato: " + fullFilePath, true);
}

bool LoadConfiguration() {
    WriteToLog("Caricamento configurazione da: " + configFile);
    
    if (!FileExists(configFile)) {
        WriteToLog("File configurazione non trovato, creazione default");
        
        std::string configDir = configFile.substr(0, configFile.find_last_of("\\/"));
        if (!CreateDirectoryRecursive(configDir)) {
            WriteToLog("ERRORE: Impossibile creare directory configurazione: " + configDir);
            return false;
        }
        
        std::ofstream config(configFile.c_str());
        if (config.is_open()) {
            config << "# PatternTriggerCommand Configuration - Multi-Folder Support\n";
            config << "# Autore: Umberto Meglio - Supporto: Claude di Anthropic\n\n";
            config << "[Settings]\n";
            config << "DefaultMonitoredFolder=" << defaultMonitoredFolder << "\n";
            config << "LogFile=" << logFile << "\n";
            config << "DetailedLogFile=" << detailedLogFile << "\n";
            config << "ProcessedFilesDB=" << processedFilesDb << "\n";
            config << "DetailedLogging=" << (detailedLogging ? "true" : "false") << "\n\n";
            config << "[Patterns]\n";
            config << "Pattern1=C:\\Monitored\\Documents|^doc.*\\..*$|C:\\Scripts\\process_doc.bat\n";
            config << "Pattern2=C:\\Monitored\\Invoices|^invoice.*\\.pdf$|C:\\Scripts\\process_invoice.bat\n";
            config << "Pattern3=^report.*\\.xlsx$|C:\\Scripts\\process_report.bat\n";
            config.close();
            
            WriteToLog("File configurazione default creato");
        } else {
            WriteToLog("ERRORE: Impossibile creare file configurazione");
            return false;
        }
    }
    
    std::ifstream config(configFile.c_str());
    if (!config.is_open()) {
        WriteToLog("ERRORE: Impossibile aprire file configurazione");
        return false;
    }
    
    std::string line;
    std::string currentSection;
    bool hasPatterns = false;
    
    patternCommandPairs.clear();
    
    while (std::getline(config, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }
        
        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) continue;
        
        std::string key = line.substr(0, equalPos);
        std::string value = line.substr(equalPos + 1);
        
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        if (currentSection == "Settings") {
            if (key == "DefaultMonitoredFolder" || key == "MonitoredFolder") {
                defaultMonitoredFolder = value;
            } else if (key == "LogFile") {
                logFile = value;
            } else if (key == "DetailedLogFile") {
                detailedLogFile = value;
            } else if (key == "ProcessedFilesDB") {
                processedFilesDb = value;
            } else if (key == "DetailedLogging") {
                detailedLogging = (value == "true" || value == "1" || value == "yes");
            }
        } else if (currentSection == "Patterns") {
            std::vector<std::string> parts;
            std::string temp = value;
            
            size_t pos = 0;
            while ((pos = temp.find('|')) != std::string::npos) {
                parts.push_back(temp.substr(0, pos));
                temp.erase(0, pos + 1);
            }
            if (!temp.empty()) {
                parts.push_back(temp);
            }
            
            std::string folderPath, pattern, command;
            
            if (parts.size() == 3) {
                folderPath = parts[0];
                pattern = parts[1];
                command = parts[2];
            } else if (parts.size() == 2) {
                folderPath = defaultMonitoredFolder;
                pattern = parts[0];
                command = parts[1];
            } else {
                WriteToLog("AVVISO: Pattern ignorato, formato non valido: " + value);
                continue;
            }
            
            folderPath.erase(0, folderPath.find_first_not_of(" \t"));
            folderPath.erase(folderPath.find_last_not_of(" \t") + 1);
            pattern.erase(0, pattern.find_first_not_of(" \t"));
            pattern.erase(pattern.find_last_not_of(" \t") + 1);
            command.erase(0, command.find_first_not_of(" \t"));
            command.erase(command.find_last_not_of(" \t") + 1);
            
            try {
                patternCommandPairs.emplace_back(folderPath, pattern, command, key);
                hasPatterns = true;
                WriteToLog("Pattern caricato: [" + key + "] '" + folderPath + 
                          "' | '" + pattern + "' | '" + command + "'", true);
            } catch (const std::regex_error& e) {
                WriteToLog("ERRORE: Pattern regex non valido '" + pattern + "': " + e.what());
            }
        }
    }
    
    config.close();
    
    if (!hasPatterns) {
        WriteToLog("ERRORE: Nessun pattern valido trovato");
        return false;
    }
    
    WriteToLog("Configurazione caricata - Pattern: " + std::to_string(patternCommandPairs.size()));
    return true;
}

std::vector<int> FindMatchingPatterns(const std::string& filename, const std::string& folderPath) {
    std::vector<int> matchingPatterns;
    std::string normalizedFolder = NormalizeFolderPath(folderPath);
    
    for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
        if (NormalizeFolderPath(patternCommandPairs[i].folderPath) == normalizedFolder) {
            try {
                if (std::regex_match(filename, patternCommandPairs[i].compiledRegex)) {
                    matchingPatterns.push_back(static_cast<int>(i));
                }
            } catch (const std::regex_error& e) {
                WriteToLog("ERRORE regex match: " + std::string(e.what()));
            }
        }
    }
    
    return matchingPatterns;
}

bool ExecuteCommand(const std::string& command, const std::string& parameter, const std::string& patternName) {
    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return false;
    
    if (!FileExists(command)) {
        WriteToLog("ERRORE: Comando non trovato: " + command);
        return false;
    }
    
    if (IsFileAlreadyProcessed(parameter)) {
        WriteToLog("SALTATO: File già processato: " + parameter);
        return false;
    }
    
    if (!WaitForFileAvailability(parameter)) {
        WriteToLog("ERRORE: File non disponibile: " + parameter);
        return false;
    }
    
    std::string commandLine = "\"" + command + "\" \"" + parameter + "\"";
    WriteToLog("ESECUZIONE [" + patternName + "]: " + commandLine);
    
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    char* cmdline = new char[commandLine.length() + 1];
    strcpy(cmdline, commandLine.c_str());
    
    if (!CreateProcess(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WriteToLog("ERRORE: CreateProcess fallito: " + std::to_string(GetLastError()));
        delete[] cmdline;
        return false;
    }
    
    HANDLE waitHandles[] = { pi.hProcess, stopEvent };
    DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, BATCH_TIMEOUT);
    bool success = false;
    
    if (waitResult == WAIT_OBJECT_0) {
        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        WriteToLog("COMPLETATO: Codice uscita " + std::to_string(exitCode));
        MarkFileAsProcessed(parameter);
        success = true;
    } else if (waitResult == WAIT_OBJECT_0 + 1) {
        WriteToLog("ANNULLATO: Terminazione richiesta");
        TerminateProcess(pi.hProcess, 1);
        success = false;
    } else {
        WriteToLog("TIMEOUT: Processo terminato forzatamente");
        TerminateProcess(pi.hProcess, 1);
        MarkFileAsProcessed(parameter);
        success = true;
    }
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    delete[] cmdline;
    
    if (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        Sleep(MONITORING_RESTART_DELAY);
    }
    
    return success;
}

void ScanDirectoryForExistingFiles(const std::string& folderPath, const std::vector<int>& patternIndices) {
    WriteToLog("Scansione cartella: " + folderPath + " (" + std::to_string(patternIndices.size()) + " pattern/s)");
    
    int filesFound = 0;
    int filesProcessed = 0;
    int filesSkipped = 0;
    
    std::string searchPath = folderPath + "\\*.*";
    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile(searchPath.c_str(), &findData);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        WriteToLog("ERRORE: Impossibile aprire cartella: " + folderPath);
        return;
    }
    
    do {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        
        filesFound++;
        std::string filename = findData.cFileName;
        std::string fullPath = folderPath + "\\" + filename;
        
        std::vector<int> matchingPatterns = FindMatchingPatterns(filename, folderPath);
        
        if (!matchingPatterns.empty()) {
            if (!IsFileAlreadyProcessed(fullPath)) {
                WriteToLog("File da elaborare: " + fullPath, true);
                
                for (int patternIndex : matchingPatterns) {
                    if (ExecuteCommand(patternCommandPairs[patternIndex].command, 
                                     fullPath, patternCommandPairs[patternIndex].patternName)) {
                        filesProcessed++;
                    }
                    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
                }
            } else {
                filesSkipped++;
            }
        }
        
    } while (FindNextFile(hFind, &findData) && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0);
    
    FindClose(hFind);
    
    WriteToLog("Scansione completata: " + folderPath + " - Trovati: " + std::to_string(filesFound) +
               ", Elaborati: " + std::to_string(filesProcessed) + ", Saltati: " + std::to_string(filesSkipped));
}

void ProcessDirectoryNotifications(BYTE* buffer, DWORD bytesTransferred, FolderMonitor* monitor) {
    FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)buffer;
    
    do {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 || 
            WaitForSingleObject(monitor->hStopEvent, 0) == WAIT_OBJECT_0) return;
        
        char filename[MAX_PATH];
        int filenameLength = WideCharToMultiByte(CP_ACP, 0, fni->FileName, 
                                               fni->FileNameLength / sizeof(WCHAR),
                                               filename, sizeof(filename), NULL, NULL);
        filename[filenameLength] = '\0';
        
        std::string strFilename(filename);
        std::string fullPath = monitor->folderPath + "\\" + strFilename;
        
        if (fni->Action == FILE_ACTION_ADDED || 
            fni->Action == FILE_ACTION_RENAMED_NEW_NAME || 
            fni->Action == FILE_ACTION_MODIFIED) {
            
            WriteToLog("Evento file: " + strFilename + " in " + monitor->folderPath, true);
            
            std::vector<int> matchingPatterns = FindMatchingPatterns(strFilename, monitor->folderPath);
            
            if (!matchingPatterns.empty() && !IsFileAlreadyProcessed(fullPath)) {
                WriteToLog("File corrispondente rilevato: " + fullPath);
                
                for (int patternIndex : matchingPatterns) {
                    if (ExecuteCommand(patternCommandPairs[patternIndex].command, 
                                     fullPath, patternCommandPairs[patternIndex].patternName)) {
                        WriteToLog("Comando eseguito per: " + fullPath, true);
                    }
                    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 || 
                        WaitForSingleObject(monitor->hStopEvent, 0) == WAIT_OBJECT_0) return;
                }
            }
        }
        
        if (fni->NextEntryOffset == 0) break;
        fni = (FILE_NOTIFY_INFORMATION*)((BYTE*)fni + fni->NextEntryOffset);
        
    } while (true);
}

DWORD WINAPI FolderMonitorThreadFunction(LPVOID lpParam) {
    FolderMonitor* monitor = static_cast<FolderMonitor*>(lpParam);
    WriteToLog("Avvio monitoraggio thread per: " + monitor->folderPath);
    monitor->active = true;
    
    HANDLE hDir = CreateFile(monitor->folderPath.c_str(), FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, 
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    
    if (hDir == INVALID_HANDLE_VALUE) {
        WriteToLog("ERRORE: Impossibile aprire directory: " + monitor->folderPath);
        monitor->active = false;
        return 1;
    }
    
    BYTE buffer[8192];
    
    while (WaitForSingleObject(monitor->hStopEvent, 0) != WAIT_OBJECT_0 && 
           WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        
        OVERLAPPED overlapped;
        ZeroMemory(&overlapped, sizeof(overlapped));
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        
        if (!overlapped.hEvent) {
            WriteToLog("ERRORE: CreateEvent fallito per: " + monitor->folderPath);
            Sleep(5000);
            continue;
        }
        
        BOOL success = ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), FALSE,
                                            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                            NULL, &overlapped, NULL);
        
        if (!success && GetLastError() != ERROR_IO_PENDING) {
            WriteToLog("ERRORE: ReadDirectoryChangesW fallito: " + std::to_string(GetLastError()));
            CloseHandle(overlapped.hEvent);
            Sleep(5000);
            continue;
        }
        
        HANDLE waitHandles[] = { overlapped.hEvent, monitor->hStopEvent, stopEvent };
        DWORD waitResult = WaitForMultipleObjects(3, waitHandles, FALSE, DIR_CHANGES_WAIT_TIMEOUT);
        
        if (waitResult == WAIT_OBJECT_0) {
            DWORD bytesTransferred = 0;
            if (GetOverlappedResult(hDir, &overlapped, &bytesTransferred, FALSE) && bytesTransferred > 0) {
                ProcessDirectoryNotifications(buffer, bytesTransferred, monitor);
            }
        } else if (waitResult == WAIT_OBJECT_0 + 1 || waitResult == WAIT_OBJECT_0 + 2) {
            WriteToLog("Terminazione richiesta per thread: " + monitor->folderPath, true);
            CloseHandle(overlapped.hEvent);
            break;
        } else if (waitResult == WAIT_TIMEOUT) {
            WriteToLog("Timeout monitoraggio: " + monitor->folderPath, true);
        }
        
        CloseHandle(overlapped.hEvent);
        Sleep(100);
    }
    
    CloseHandle(hDir);
    monitor->active = false;
    WriteToLog("Thread monitoraggio terminato per: " + monitor->folderPath);
    return 0;
}

void StartAllFolderMonitors() {
    // Raggruppa pattern per cartella
    std::map<std::string, std::vector<int>> folderPatterns;
    for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
        std::string normalizedFolder = NormalizeFolderPath(patternCommandPairs[i].folderPath);
        folderPatterns[normalizedFolder].push_back(static_cast<int>(i));
    }
    
    WriteToLog("Avvio monitoraggio per " + std::to_string(folderPatterns.size()) + " cartelle");
    
    // Scansione iniziale e avvio monitoraggio per ogni cartella
    for (const auto& folderGroup : folderPatterns) {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        
        std::string originalFolder = patternCommandPairs[folderGroup.second[0]].folderPath;
        
        // Verifica/crea directory
        if (!DirectoryExists(originalFolder)) {
            if (!CreateDirectoryRecursive(originalFolder)) {
                WriteToLog("ERRORE: Impossibile creare directory: " + originalFolder);
                continue;
            }
        }
        
        // Scansione iniziale
        ScanDirectoryForExistingFiles(originalFolder, folderGroup.second);
        
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        
        // Crea monitor per la cartella
        std::unique_ptr<FolderMonitor> monitor(new FolderMonitor(originalFolder));
        monitor->patternIndices = folderGroup.second;
        
        // Crea evento di stop per questo thread
        monitor->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!monitor->hStopEvent) {
            WriteToLog("ERRORE: Impossibile creare evento stop per cartella: " + originalFolder);
            continue;
        }
        
        // Avvia thread di monitoraggio
        monitor->hThread = CreateThread(NULL, 0, FolderMonitorThreadFunction, 
                                       monitor.get(), 0, &monitor->threadId);
        
        if (!monitor->hThread) {
            WriteToLog("ERRORE: Impossibile creare thread per cartella: " + originalFolder);
            CloseHandle(monitor->hStopEvent);
            continue;
        }
        
        WriteToLog("Monitor avviato per: " + originalFolder + " (TID: " + 
                  std::to_string(monitor->threadId) + ")");
        
        folderMonitors[folderGroup.first] = std::move(monitor);
    }
    
    WriteToLog("Tutti i monitor avviati. Thread attivi: " + std::to_string(folderMonitors.size()));
}

void StopAllFolderMonitors() {
    WriteToLog("Arresto di tutti i monitor cartelle...");
    
    // Segnala a tutti i thread di fermarsi
    for (auto& monitorPair : folderMonitors) {
        if (monitorPair.second->hStopEvent) {
            SetEvent(monitorPair.second->hStopEvent);
        }
    }
    
    // Attendi terminazione thread
    std::vector<HANDLE> threadHandles;
    for (const auto& monitorPair : folderMonitors) {
        if (monitorPair.second->hThread) {
            threadHandles.push_back(monitorPair.second->hThread);
        }
    }
    
    if (!threadHandles.empty()) {
        DWORD waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(threadHandles.size()), 
            threadHandles.data(), TRUE, 10000
        );
        
        if (waitResult == WAIT_TIMEOUT) {
            WriteToLog("AVVISO: Alcuni thread non si sono terminati entro il timeout");
        } else {
            WriteToLog("Tutti i thread terminati correttamente");
        }
    }
    
    folderMonitors.clear();
    WriteToLog("Tutti i monitor sono stati fermati");
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam) {
    WriteToLog("=== Servizio PatternTriggerCommand Multi-Folder avviato ===");
    
    // Carica configurazione
    if (!LoadConfiguration()) {
        WriteToLog("ERRORE FATALE: Impossibile caricare configurazione");
        return 1;
    }
    
    // Crea directory necessarie
    std::string scriptsDir = "C:\\Scripts";
    CreateDirectoryRecursive(scriptsDir);
    
    // Carica file processati
    LoadProcessedFiles();
    
    // Pulisci cache ignorati
    recentlyIgnoredFiles.clear();
    
    // Avvia tutti i monitor delle cartelle
    StartAllFolderMonitors();
    
    // Loop principale - attende solo la terminazione
    WriteToLog("Servizio in esecuzione, attesa terminazione...");
    
    while (WaitForSingleObject(stopEvent, 10000) == WAIT_TIMEOUT) {
        // Verifica periodica stato monitor
        int activeMonitors = 0;
        for (const auto& monitorPair : folderMonitors) {
            if (monitorPair.second->active) activeMonitors++;
        }
        WriteToLog("Monitor attivi: " + std::to_string(activeMonitors), true);
        
        // Pulizia periodica cache file ignorati
        static DWORD lastCleanup = GetTickCount();
        if (GetTickCount() - lastCleanup > CACHE_CLEANUP_INTERVAL) {
            recentlyIgnoredFiles.clear();
            lastCleanup = GetTickCount();
            WriteToLog("Pulizia cache file ignorati", true);
        }
    }
    
    // Cleanup
    WriteToLog("Terminazione richiesta, cleanup in corso...");
    StopAllFolderMonitors();
    SaveProcessedFiles();
    
    WriteToLog("=== Servizio PatternTriggerCommand terminato ===");
    return 0;
}

void WINAPI ServiceCtrlHandler(DWORD ctrlCode) {
    switch (ctrlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            WriteToLog("Richiesta arresto servizio ricevuta");
            
            serviceStatus.dwControlsAccepted = 0;
            serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            serviceStatus.dwWin32ExitCode = 0;
            serviceStatus.dwCheckPoint = 4;
            serviceStatus.dwWaitHint = SERVICE_SHUTDOWN_TIMEOUT;
            
            SetServiceStatus(serviceStatusHandle, &serviceStatus);
            
            if (stopEvent) {
                SetEvent(stopEvent);
            }
            break;
            
        default:
            break;
    }
}

void WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    serviceStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);
    
    if (serviceStatusHandle == NULL) {
        WriteToLog("RegisterServiceCtrlHandler fallito");
        return;
    }
    
    ZeroMemory(&serviceStatus, sizeof(serviceStatus));
    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    serviceStatus.dwWin32ExitCode = 0;
    serviceStatus.dwServiceSpecificExitCode = 0;
    serviceStatus.dwCheckPoint = 0;
    serviceStatus.dwWaitHint = 0;
    
    SetServiceStatus(serviceStatusHandle, &serviceStatus);
    
    // Crea evento di controllo servizio
    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stopEvent == NULL) {
        WriteToLog("CreateEvent fallito");
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        serviceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(serviceStatusHandle, &serviceStatus);
        return;
    }
    
    // Avvia thread di servizio
    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (hThread == NULL) {
        WriteToLog("CreateThread fallito");
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        serviceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(serviceStatusHandle, &serviceStatus);
        return;
    }
    
    serviceStatus.dwCurrentState = SERVICE_RUNNING;
    serviceStatus.dwCheckPoint = 0;
    serviceStatus.dwWaitHint = 0;
    
    SetServiceStatus(serviceStatusHandle, &serviceStatus);
    
    // Attendi terminazione thread
    WaitForSingleObject(hThread, INFINITE);
    
    // Cleanup
    CloseHandle(hThread);
    CloseHandle(stopEvent);
    
    serviceStatus.dwCurrentState = SERVICE_STOPPED;
    serviceStatus.dwCheckPoint = 0;
    serviceStatus.dwWaitHint = 0;
    
    SetServiceStatus(serviceStatusHandle, &serviceStatus);
    WriteToLog("Servizio terminato");
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        std::cout << "\nInterruzione richiesta, attendere..." << std::endl;
        if (stopEvent) {
            SetEvent(stopEvent);
        }
        return TRUE;
    }
    return FALSE;
}

bool ShouldLogIgnoredFile(const std::string& filename) {
    if (recentlyIgnoredFiles.find(filename) == recentlyIgnoredFiles.end()) {
        recentlyIgnoredFiles.insert(filename);
        return true;
    }
    return false;
}

int main(int argc, char* argv[]) {
    std::string configFileStr = DEFAULT_CONFIG_FILE;
    std::string baseDir = configFileStr.substr(0, configFileStr.find_last_of("\\/"));
    CreateDirectoryRecursive(baseDir);
    
    WriteToLog("=== PatternTriggerCommand Multi-Folder ===");
    WriteToLog("Autore: Umberto Meglio - Supporto: Claude di Anthropic");
    
    // Gestione parametri
    if (argc > 1) {
        std::string command = argv[1];
        
        if (command == "install") {
            WriteToLog("Installazione servizio...");
            
            SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
            if (schSCManager == NULL) {
                std::cerr << "OpenSCManager fallito: " << GetLastError() << std::endl;
                return 1;
            }
            
            char path[MAX_PATH];
            GetModuleFileName(NULL, path, MAX_PATH);
            
            SC_HANDLE schService = CreateService(schSCManager, SERVICE_NAME, SERVICE_DISPLAY_NAME,
                                                SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                                SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                                path, NULL, NULL, NULL, NULL, NULL);
            
            if (schService == NULL) {
                std::cerr << "CreateService fallito: " << GetLastError() << std::endl;
                CloseServiceHandle(schSCManager);
                return 1;
            }
            
            SERVICE_DESCRIPTION_STRUCT sd;
            sd.lpDescription = const_cast<LPSTR>(SERVICE_DESCRIPTION);
            MyChangeServiceConfig2(schService, SERVICE_CONFIG_DESCRIPTION, &sd);
            
            std::cout << "Servizio installato con successo." << std::endl;
            WriteToLog("Servizio installato");
            
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
        }
        else if (command == "uninstall") {
            WriteToLog("Disinstallazione servizio...");
            
            SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
            if (schSCManager == NULL) {
                std::cerr << "OpenSCManager fallito: " << GetLastError() << std::endl;
                return 1;
            }
            
            SC_HANDLE schService = OpenService(schSCManager, SERVICE_NAME, DELETE);
            if (schService == NULL) {
                std::cerr << "OpenService fallito: " << GetLastError() << std::endl;
                CloseServiceHandle(schSCManager);
                return 1;
            }
            
            if (!DeleteService(schService)) {
                std::cerr << "DeleteService fallito: " << GetLastError() << std::endl;
                CloseServiceHandle(schService);
                CloseServiceHandle(schSCManager);
                return 1;
            }
            
            std::cout << "Servizio disinstallato con successo." << std::endl;
            WriteToLog("Servizio disinstallato");
            
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
        }
        else if (command == "test") {
            WriteToLog("Modalità test console");
            std::cout << "Test servizio in modalità console..." << std::endl;
            
            if (!LoadConfiguration()) {
                std::cerr << "Errore caricamento configurazione" << std::endl;
                return 1;
            }
            
            std::cout << "Configurazione caricata con " << patternCommandPairs.size() << " pattern/s" << std::endl;
            
            // Raggruppa pattern per cartella per visualizzazione
            std::map<std::string, std::vector<int>> folderPatterns;
            for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
                folderPatterns[patternCommandPairs[i].folderPath].push_back(static_cast<int>(i));
            }
            
            std::cout << "Cartelle monitorate: " << folderPatterns.size() << std::endl;
            for (const auto& folderGroup : folderPatterns) {
                std::cout << "  " << folderGroup.first << " (" << folderGroup.second.size() << " pattern/s)" << std::endl;
                for (int patternIndex : folderGroup.second) {
                    std::cout << "    [" << patternCommandPairs[patternIndex].patternName << "] '" 
                              << patternCommandPairs[patternIndex].patternRegex 
                              << "' -> '" << patternCommandPairs[patternIndex].command << "'" << std::endl;
                }
            }
            
            std::cout << "Premi CTRL+C per terminare..." << std::endl;
            
            // Crea evento per test
            stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (stopEvent == NULL) {
                std::cerr << "CreateEvent fallito" << std::endl;
                return 1;
            }
            
            SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
            
            ServiceWorkerThread(NULL);
            
            CloseHandle(stopEvent);
        }
        else if (command == "status") {
            LoadConfiguration();
            LoadProcessedFiles();
            
            std::cout << "=== Status PatternTriggerCommand ===" << std::endl;
            std::cout << "File processati: " << processedFiles.size() << std::endl;
            
            // Verifica stato servizio
            SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
            if (schSCManager) {
                SC_HANDLE schService = OpenService(schSCManager, SERVICE_NAME, SERVICE_QUERY_STATUS);
                if (schService) {
                    SERVICE_STATUS_PROCESS ssp;
                    DWORD bytesNeeded;
                    if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, 
                                            (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
                        std::cout << "Stato servizio: ";
                        switch (ssp.dwCurrentState) {
                            case SERVICE_STOPPED: std::cout << "FERMATO"; break;
                            case SERVICE_START_PENDING: std::cout << "AVVIO IN CORSO"; break;
                            case SERVICE_STOP_PENDING: std::cout << "ARRESTO IN CORSO"; break;
                            case SERVICE_RUNNING: std::cout << "IN ESECUZIONE"; break;
                            default: std::cout << "SCONOSCIUTO"; break;
                        }
                        std::cout << std::endl;
                    }
                    CloseServiceHandle(schService);
                } else {
                    std::cout << "Stato servizio: NON INSTALLATO" << std::endl;
                }
                CloseServiceHandle(schSCManager);
            }
            
            // Mostra configurazione
            std::map<std::string, int> folderCounts;
            for (const auto& pair : patternCommandPairs) {
                folderCounts[pair.folderPath]++;
            }
            
            std::cout << "Cartelle monitorate: " << folderCounts.size() << std::endl;
            for (const auto& folder : folderCounts) {
                std::cout << "  " << folder.first << " (" << folder.second << " pattern/s)" << std::endl;
            }
        }
        else if (command == "reset") {
            LoadConfiguration();
            WriteToLog("Reset database file processati");
            
            std::ofstream file(processedFilesDb.c_str(), std::ios::trunc);
            if (file.is_open()) {
                file.close();
                std::cout << "Database reset completato." << std::endl;
                WriteToLog("Database reset");
            } else {
                std::cerr << "Errore reset database." << std::endl;
            }
        }
        else if (command == "config") {
            WriteToLog("Creazione/aggiornamento configurazione");
            LoadConfiguration();
            std::cout << "Configurazione creata/aggiornata." << std::endl;
        }
        else if (command == "reprocess" && argc > 3) {
            LoadConfiguration();
            std::string folderPath = argv[2];
            std::string filename = argv[3];
            std::string fullPath = folderPath + "\\" + filename;
            
            if (FileExists(fullPath)) {
                LoadProcessedFiles();
                
                processedFiles.erase(fullPath);
                SaveProcessedFiles();
                
                std::vector<int> matchingPatterns = FindMatchingPatterns(filename, folderPath);
                if (!matchingPatterns.empty()) {
                    for (int patternIndex : matchingPatterns) {
                        ExecuteCommand(patternCommandPairs[patternIndex].command, 
                                     fullPath, patternCommandPairs[patternIndex].patternName);
                    }
                    std::cout << "File riprocessato: " << fullPath << std::endl;
                } else {
                    std::cerr << "Nessun pattern corrispondente per il file." << std::endl;
                }
            } else {
                std::cerr << "File non trovato: " << fullPath << std::endl;
            }
        }
        else {
            std::cerr << "Comando non riconosciuto: " << command << std::endl;
            std::cerr << "Comandi disponibili:" << std::endl;
            std::cerr << "  install    - installa servizio" << std::endl;
            std::cerr << "  uninstall  - disinstalla servizio" << std::endl;
            std::cerr << "  test       - modalità console" << std::endl;
            std::cerr << "  status     - stato servizio" << std::endl;
            std::cerr << "  reset      - reset database" << std::endl;
            std::cerr << "  config     - crea configurazione" << std::endl;
            std::cerr << "  reprocess <cartella> <file> - riprocessa file" << std::endl;
            return 1;
        }
    }
    else {
        // Avvia come servizio Windows
        WriteToLog("Avvio come servizio Windows");
        
        SERVICE_TABLE_ENTRY serviceTable[] = {
            { const_cast<LPSTR>(SERVICE_NAME), (LPSERVICE_MAIN_FUNCTION)ServiceMain },
            { NULL, NULL }
        };
        
        if (StartServiceCtrlDispatcher(serviceTable) == FALSE) {
            std::cerr << "Errore: Eseguire come servizio Windows o con parametri." << std::endl;
            std::cerr << "Per aiuto: PatternTriggerCommand.exe help" << std::endl;
            return 1;
        }
    }
    
    return 0;
}
