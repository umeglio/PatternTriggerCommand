#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
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
#include <thread>
#include <mutex>
#include <atomic>
#include <sstream>
#include <chrono>

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
#define SERVICE_DISPLAY_NAME "Pattern Trigger Command Service Multi-Folder"
#define SERVICE_DESCRIPTION "Monitora cartelle multiple per file che corrispondono a pattern configurati ed esegue comandi associati con dashboard web integrata"

// Percorsi predefiniti
#define DEFAULT_MONITORED_FOLDER "C:\\Monitored"
#define DEFAULT_CONFIG_FILE "C:\\PTC\\config.ini"
#define DEFAULT_LOG_FILE "C:\\PTC\\PatternTriggerCommand.log"
#define DEFAULT_DETAILED_LOG_FILE "C:\\PTC\\PatternTriggerCommand_detailed.log"
#define DEFAULT_PROCESSED_FILES_DB "C:\\PTC\\PatternTriggerCommand_processed.txt"
#define DEFAULT_WEB_PORT 8080

// Intervalli di tempo ottimizzati
#define FILE_CHECK_INTERVAL 1000
#define MONITORING_RESTART_DELAY 1000
#define BATCH_TIMEOUT 45000
#define CACHE_CLEANUP_INTERVAL 180000
#define SERVICE_SHUTDOWN_TIMEOUT 8000
#define WEB_UPDATE_INTERVAL 2000
#define METRICS_UPDATE_INTERVAL 5000

// Variabili globali del servizio
SERVICE_STATUS serviceStatus;
SERVICE_STATUS_HANDLE serviceStatusHandle;
std::atomic<bool> globalShutdown(false);
HANDLE stopEvent = NULL;

// Mutex per thread safety
std::mutex logMutex;
std::mutex configMutex;
std::mutex processedFilesMutex;
std::mutex metricsMutex;
std::mutex patternStatsMutex;

// Configurazione
std::string defaultMonitoredFolder = DEFAULT_MONITORED_FOLDER;
std::string configFile = DEFAULT_CONFIG_FILE;
std::string logFile = DEFAULT_LOG_FILE;
std::string detailedLogFile = DEFAULT_DETAILED_LOG_FILE;
std::string processedFilesDb = DEFAULT_PROCESSED_FILES_DB;
bool detailedLogging = true;
int webServerPort = DEFAULT_WEB_PORT;
bool webServerEnabled = true;

// Statistiche pattern (separate dalla struct per evitare problemi di move)
std::map<std::string, size_t> patternMatchCounts;
std::map<std::string, size_t> patternExecutionCounts;

// Metriche di sistema
struct SystemMetrics {
    std::atomic<size_t> totalFilesProcessed{0};
    std::atomic<size_t> filesProcessedToday{0};
    std::atomic<size_t> activeThreads{0};
    std::atomic<size_t> memoryUsageMB{0};
    std::atomic<size_t> averageProcessingTime{0};
    std::atomic<size_t> commandsExecuted{0};
    std::atomic<size_t> errorsCount{0};
    std::chrono::steady_clock::time_point serviceStartTime;
    std::chrono::steady_clock::time_point lastFileProcessed;
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> recentActivity;
    
    SystemMetrics() : serviceStartTime(std::chrono::steady_clock::now()) {}
} systemMetrics;

// Struttura per pattern e comandi (senza atomic per evitare problemi di move)
struct PatternCommandPair {
    std::string folderPath;
    std::string patternRegex;
    std::string command;
    std::regex compiledRegex;
    std::string patternName;
    
    PatternCommandPair(const std::string& folder, const std::string& pattern, 
                      const std::string& cmd, const std::string& name = "") 
        : folderPath(folder), patternRegex(pattern), command(cmd), 
          compiledRegex(pattern, std::regex_constants::icase), patternName(name) {
        // Inizializza contatori pattern
        std::lock_guard<std::mutex> lock(patternStatsMutex);
        patternMatchCounts[patternName] = 0;
        patternExecutionCounts[patternName] = 0;
    }
};

std::vector<PatternCommandPair> patternCommandPairs;

// Gestione dei file processati con thread safety
std::set<std::string> processedFiles;
std::set<std::string> recentlyIgnoredFiles;

// Struttura per monitoraggio cartella
struct FolderMonitor {
    std::string folderPath;
    std::string normalizedPath;
    std::vector<int> patternIndices;
    std::atomic<bool> active;
    std::atomic<bool> stopRequested;
    std::thread workerThread;
    HANDLE directoryHandle;
    std::atomic<size_t> filesDetected{0};
    std::atomic<size_t> filesProcessed{0};
    
    FolderMonitor(const std::string& path) : folderPath(path), active(false), 
        stopRequested(false), directoryHandle(INVALID_HANDLE_VALUE) {
        normalizedPath = path;
        std::replace(normalizedPath.begin(), normalizedPath.end(), '/', '\\');
        if (!normalizedPath.empty() && normalizedPath.back() == '\\') {
            normalizedPath.pop_back();
        }
        std::transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(), ::toupper);
    }
    
    ~FolderMonitor() {
        StopMonitoring();
        if (directoryHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(directoryHandle);
        }
    }
    
    void StopMonitoring() {
        stopRequested = true;
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }
};

std::map<std::string, std::unique_ptr<FolderMonitor>> folderMonitors;

// Web Server
std::thread webServerThread;
std::atomic<bool> webServerRunning{false};
std::atomic<bool> webServerShouldStop{false};

// ====== DICHIARAZIONI FUNZIONI ======

std::string GetTimestamp();
void WriteToLog(const std::string& message, bool detailed = false);
std::string NormalizeFolderPath(const std::string& path);
std::string EscapeJsonString(const std::string& input);
bool FileExists(const std::string& filename);
bool DirectoryExists(const std::string& path);
bool CreateDirectoryRecursive(const std::string& path);
bool IsFileInUse(const std::string& filePath);
bool WaitForFileAvailability(const std::string& filePath, int maxWaitTimeMs = 20000);
void LoadProcessedFiles();
void SaveProcessedFiles();
bool IsFileAlreadyProcessed(const std::string& fullFilePath);
void MarkFileAsProcessed(const std::string& fullFilePath);
bool LoadConfiguration();
std::vector<int> FindMatchingPatterns(const std::string& filename, const std::string& folderPath);
bool ExecuteCommand(const std::string& command, const std::string& parameter, const std::string& patternName);
void ScanDirectoryForExistingFiles(const std::string& folderPath, const std::vector<int>& patternIndices);
void FolderMonitorWorker(FolderMonitor* monitor);
void StartAllFolderMonitors();
void StopAllFolderMonitors();
void UpdateSystemMetrics();
void MetricsUpdateWorker();
std::string GetSystemMetricsJson();
std::string GetDashboardHtml();
std::string HandleHttpRequest(const std::string& request);
void WebServerWorker();
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
    std::lock_guard<std::mutex> lock(logMutex);
    
    std::ofstream logFileStream(logFile.c_str(), std::ios::app);
    if (logFileStream.is_open()) {
        logFileStream << GetTimestamp() << " - " << message << std::endl;
        logFileStream.close();
    }
    
    if (detailed && detailedLogging) {
        std::ofstream detailedLogFileStream(detailedLogFile.c_str(), std::ios::app);
        if (detailedLogFileStream.is_open()) {
            detailedLogFileStream << GetTimestamp() << " - [DETAILED] " << message << std::endl;
            detailedLogFileStream.close();
        }
    }
    
    // Aggiorna attività recente per dashboard
    {
        std::lock_guard<std::mutex> metricsLock(metricsMutex);
        systemMetrics.recentActivity.push_back({message, std::chrono::steady_clock::now()});
        if (systemMetrics.recentActivity.size() > 20) {
            systemMetrics.recentActivity.erase(systemMetrics.recentActivity.begin());
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

std::string EscapeJsonString(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.length() + 20); // Pre-alloca spazio extra
    
    for (char c : input) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    // Caratteri di controllo
                    escaped += "\\u";
                    escaped += "0000";
                    escaped[escaped.length()-2] = "0123456789ABCDEF"[(c >> 4) & 0xF];
                    escaped[escaped.length()-1] = "0123456789ABCDEF"[c & 0xF];
                } else {
                    escaped += c;
                }
                break;
        }
    }
    return escaped;
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
    const int sleepInterval = 250;
    
    WriteToLog("Verifica disponibilità file: " + filePath, true);
    
    while (waitTime < maxWaitTimeMs && !globalShutdown) {
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
    
    if (globalShutdown) {
        WriteToLog("Terminazione richiesta durante attesa file", true);
    }
    
    return false;
}

void LoadProcessedFiles() {
    std::lock_guard<std::mutex> lock(processedFilesMutex);
    
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
        systemMetrics.totalFilesProcessed = processedFiles.size();
    } else {
        WriteToLog("Database file processati non trovato, verrà creato");
    }
}

void SaveProcessedFiles() {
    std::lock_guard<std::mutex> lock(processedFilesMutex);
    
    std::ofstream file(processedFilesDb.c_str());
    if (file.is_open()) {
        for (const auto& filename : processedFiles) {
            file << filename << std::endl;
        }
        file.close();
        WriteToLog("Salvati " + std::to_string(processedFiles.size()) + " file nel database", true);
    } else {
        WriteToLog("ERRORE: Impossibile salvare database file processati");
        systemMetrics.errorsCount++;
    }
}

bool IsFileAlreadyProcessed(const std::string& fullFilePath) {
    std::lock_guard<std::mutex> lock(processedFilesMutex);
    return processedFiles.find(fullFilePath) != processedFiles.end();
}

void MarkFileAsProcessed(const std::string& fullFilePath) {
    {
        std::lock_guard<std::mutex> lock(processedFilesMutex);
        processedFiles.insert(fullFilePath);
    }
    SaveProcessedFiles();
    WriteToLog("File marcato come processato: " + fullFilePath, true);
    
    systemMetrics.totalFilesProcessed++;
    systemMetrics.filesProcessedToday++;
    systemMetrics.lastFileProcessed = std::chrono::steady_clock::now();
}

bool LoadConfiguration() {
    std::lock_guard<std::mutex> lock(configMutex);
    
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
            config << "# PatternTriggerCommand Configuration - Multi-Folder Support + Web Dashboard\n";
            config << "# Autore: Umberto Meglio - Supporto: Claude di Anthropic\n\n";
            config << "[Settings]\n";
            config << "DefaultMonitoredFolder=" << defaultMonitoredFolder << "\n";
            config << "LogFile=" << logFile << "\n";
            config << "DetailedLogFile=" << detailedLogFile << "\n";
            config << "ProcessedFilesDB=" << processedFilesDb << "\n";
            config << "DetailedLogging=" << (detailedLogging ? "true" : "false") << "\n";
            config << "WebServerPort=" << webServerPort << "\n";
            config << "WebServerEnabled=" << (webServerEnabled ? "true" : "false") << "\n\n";
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
            } else if (key == "WebServerPort") {
                webServerPort = std::stoi(value);
            } else if (key == "WebServerEnabled") {
                webServerEnabled = (value == "true" || value == "1" || value == "yes");
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
                systemMetrics.errorsCount++;
            }
        }
    }
    
    config.close();
    
    if (!hasPatterns) {
        WriteToLog("ERRORE: Nessun pattern valido trovato");
        return false;
    }
    
    WriteToLog("Configurazione caricata - Pattern: " + std::to_string(patternCommandPairs.size()) + 
               ", WebServer: " + (webServerEnabled ? "abilitato" : "disabilitato") + 
               " porta " + std::to_string(webServerPort));
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
                    
                    // Aggiorna contatore match
                    std::lock_guard<std::mutex> lock(patternStatsMutex);
                    patternMatchCounts[patternCommandPairs[i].patternName]++;
                }
            } catch (const std::regex_error& e) {
                WriteToLog("ERRORE regex match: " + std::string(e.what()));
                systemMetrics.errorsCount++;
            }
        }
    }
    
    return matchingPatterns;
}

bool ExecuteCommand(const std::string& command, const std::string& parameter, const std::string& patternName) {
    if (globalShutdown) return false;
    
    auto startTime = std::chrono::steady_clock::now();
    
    if (!FileExists(command)) {
        WriteToLog("ERRORE: Comando non trovato: " + command);
        systemMetrics.errorsCount++;
        return false;
    }
    
    if (IsFileAlreadyProcessed(parameter)) {
        WriteToLog("SALTATO: File già processato: " + parameter);
        return false;
    }
    
    if (!WaitForFileAvailability(parameter)) {
        WriteToLog("ERRORE: File non disponibile: " + parameter);
        systemMetrics.errorsCount++;
        return false;
    }
    
    std::string commandLine = "\"" + command + "\" \"" + parameter + "\"";
    WriteToLog("ESECUZIONE [" + patternName + "]: " + commandLine);
    
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));
    
    char* cmdline = new char[commandLine.length() + 1];
    strcpy(cmdline, commandLine.c_str());
    
    if (!CreateProcess(NULL, cmdline, NULL, NULL, FALSE, 
                      CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WriteToLog("ERRORE: CreateProcess fallito: " + std::to_string(GetLastError()));
        systemMetrics.errorsCount++;
        delete[] cmdline;
        return false;
    }
    
    DWORD waitResult = WaitForSingleObject(pi.hProcess, BATCH_TIMEOUT);
    bool success = false;
    
    if (waitResult == WAIT_OBJECT_0) {
        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        WriteToLog("COMPLETATO: Codice uscita " + std::to_string(exitCode));
        MarkFileAsProcessed(parameter);
        success = true;
        systemMetrics.commandsExecuted++;
        
        // Aggiorna contatore esecuzioni
        std::lock_guard<std::mutex> lock(patternStatsMutex);
        patternExecutionCounts[patternName]++;
        
    } else if (waitResult == WAIT_TIMEOUT) {
        WriteToLog("TIMEOUT: Processo terminato forzatamente");
        TerminateProcess(pi.hProcess, 1);
        MarkFileAsProcessed(parameter);
        success = true;
        systemMetrics.commandsExecuted++;
        
        std::lock_guard<std::mutex> lock(patternStatsMutex);
        patternExecutionCounts[patternName]++;
        
    } else {
        WriteToLog("ERRORE: Attesa processo fallita");
        TerminateProcess(pi.hProcess, 1);
        systemMetrics.errorsCount++;
        success = false;
    }
    
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    systemMetrics.averageProcessingTime = (systemMetrics.averageProcessingTime + duration) / 2;
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    delete[] cmdline;
    
    if (!globalShutdown) {
        Sleep(MONITORING_RESTART_DELAY);
    }
    
    return success;
}

void ScanDirectoryForExistingFiles(const std::string& folderPath, const std::vector<int>& patternIndices) {
    WriteToLog("Scansione iniziale cartella: " + folderPath + " (" + std::to_string(patternIndices.size()) + " pattern/s)");
    
    int filesFound = 0;
    int filesProcessed = 0;
    int filesSkipped = 0;
    
    std::string searchPath = folderPath + "\\*.*";
    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile(searchPath.c_str(), &findData);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        WriteToLog("ERRORE: Impossibile aprire cartella: " + folderPath);
        systemMetrics.errorsCount++;
        return;
    }
    
    do {
        if (globalShutdown) break;
        
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        
        filesFound++;
        std::string filename = findData.cFileName;
        std::string fullPath = folderPath + "\\" + filename;
        
        // CORREZIONE: Processa TUTTI i file che matchano i pattern, anche se già processati
        std::vector<int> matchingPatterns = FindMatchingPatterns(filename, folderPath);
        
        if (!matchingPatterns.empty()) {
            bool alreadyProcessed = IsFileAlreadyProcessed(fullPath);
            
            if (!alreadyProcessed) {
                WriteToLog("File NON processato trovato: " + fullPath);
                
                for (int patternIndex : matchingPatterns) {
                    if (ExecuteCommand(patternCommandPairs[patternIndex].command, 
                                     fullPath, patternCommandPairs[patternIndex].patternName)) {
                        filesProcessed++;
                        WriteToLog("File processato durante scansione: " + fullPath);
                    }
                    if (globalShutdown) break;
                }
            } else {
                filesSkipped++;
                WriteToLog("File già processato saltato: " + fullPath, true);
            }
        }
        
    } while (FindNextFile(hFind, &findData) && !globalShutdown);
    
    FindClose(hFind);
    
    WriteToLog("Scansione iniziale completata: " + folderPath + 
               " - Trovati: " + std::to_string(filesFound) +
               ", Nuovi processati: " + std::to_string(filesProcessed) + 
               ", Già processati: " + std::to_string(filesSkipped));
}

void FolderMonitorWorker(FolderMonitor* monitor) {
    WriteToLog("Avvio monitoraggio worker per: " + monitor->folderPath);
    monitor->active = true;
    systemMetrics.activeThreads++;
    
    monitor->directoryHandle = CreateFile(monitor->folderPath.c_str(), FILE_LIST_DIRECTORY,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         NULL, OPEN_EXISTING, 
                                         FILE_FLAG_BACKUP_SEMANTICS, NULL);
    
    if (monitor->directoryHandle == INVALID_HANDLE_VALUE) {
        WriteToLog("ERRORE: Impossibile aprire directory: " + monitor->folderPath + 
                  " Error: " + std::to_string(GetLastError()));
        monitor->active = false;
        systemMetrics.activeThreads--;
        systemMetrics.errorsCount++;
        return;
    }
    
    BYTE buffer[4096];
    DWORD bytesRead = 0;
    
    while (!monitor->stopRequested && !globalShutdown) {
        // CORREZIONE: Controlla se handle è ancora valido
        if (monitor->directoryHandle == INVALID_HANDLE_VALUE) {
            WriteToLog("Handle directory chiuso - terminazione thread: " + monitor->folderPath, true);
            break;
        }
        
        BOOL result = ReadDirectoryChangesW(
            monitor->directoryHandle,
            buffer,
            sizeof(buffer),
            FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesRead,
            NULL,
            NULL
        );
        
        if (!result) {
            DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED || error == ERROR_INVALID_HANDLE || error == ERROR_ACCESS_DENIED) {
                WriteToLog("Monitoraggio interrotto per: " + monitor->folderPath + " (Error: " + std::to_string(error) + ")", true);
                break;
            } else {
                WriteToLog("ERRORE ReadDirectoryChangesW: " + std::to_string(error) + 
                          " per cartella: " + monitor->folderPath);
                systemMetrics.errorsCount++;
                
                // CORREZIONE: Non loop infinito su errori persistenti
                if (monitor->stopRequested || globalShutdown) break;
                Sleep(1000);
                continue;
            }
        }
        
        if (bytesRead == 0 || monitor->stopRequested || globalShutdown) {
            continue;
        }
        
        FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)buffer;
        
        do {
            if (monitor->stopRequested || globalShutdown) break;
            
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
                monitor->filesDetected++;
                
                Sleep(500);
                
                std::vector<int> matchingPatterns = FindMatchingPatterns(strFilename, monitor->folderPath);
                
                if (!matchingPatterns.empty() && !IsFileAlreadyProcessed(fullPath)) {
                    WriteToLog("File corrispondente rilevato: " + fullPath);
                    
                    for (int patternIndex : matchingPatterns) {
                        if (ExecuteCommand(patternCommandPairs[patternIndex].command, 
                                         fullPath, patternCommandPairs[patternIndex].patternName)) {
                            WriteToLog("Comando eseguito per: " + fullPath, true);
                            monitor->filesProcessed++;
                        }
                        if (monitor->stopRequested || globalShutdown) break;
                    }
                }
            }
            
            if (fni->NextEntryOffset == 0) break;
            fni = (FILE_NOTIFY_INFORMATION*)((BYTE*)fni + fni->NextEntryOffset);
            
        } while (!monitor->stopRequested && !globalShutdown);
        
        // CORREZIONE: Check più frequenti per essere più reattivo
        Sleep(50);
    }
    
    // CORREZIONE: Cleanup sicuro dell'handle
    if (monitor->directoryHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(monitor->directoryHandle);
        monitor->directoryHandle = INVALID_HANDLE_VALUE;
    }
    
    monitor->active = false;
    systemMetrics.activeThreads--;
    WriteToLog("Worker monitoraggio terminato per: " + monitor->folderPath);
}

void StartAllFolderMonitors() {
    std::map<std::string, std::vector<int>> folderPatterns;
    for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
        std::string normalizedFolder = NormalizeFolderPath(patternCommandPairs[i].folderPath);
        folderPatterns[normalizedFolder].push_back(static_cast<int>(i));
    }
    
    WriteToLog("Avvio monitoraggio per " + std::to_string(folderPatterns.size()) + " cartelle");
    
    for (const auto& folderGroup : folderPatterns) {
        if (globalShutdown) break;
        
        std::string originalFolder = patternCommandPairs[folderGroup.second[0]].folderPath;
        
        if (!DirectoryExists(originalFolder)) {
            if (!CreateDirectoryRecursive(originalFolder)) {
                WriteToLog("ERRORE: Impossibile creare directory: " + originalFolder);
                systemMetrics.errorsCount++;
                continue;
            }
        }
        
        // CORREZIONE: Scansione iniziale di TUTTI i file esistenti
        WriteToLog("=== SCANSIONE INIZIALE CARTELLA: " + originalFolder + " ===");
        ScanDirectoryForExistingFiles(originalFolder, folderGroup.second);
        
        if (globalShutdown) break;
        
        std::unique_ptr<FolderMonitor> monitor(new FolderMonitor(originalFolder));
        monitor->patternIndices = folderGroup.second;
        
        monitor->workerThread = std::thread(FolderMonitorWorker, monitor.get());
        
        WriteToLog("Monitor avviato per: " + originalFolder);
        
        folderMonitors[folderGroup.first] = std::move(monitor);
        
        Sleep(500);
    }
    
    WriteToLog("Tutti i monitor avviati. Thread attivi: " + std::to_string(folderMonitors.size()));
}

void StopAllFolderMonitors() {
    WriteToLog("Arresto di tutti i monitor cartelle...");
    
    // Fase 1: Segnala stop a tutti
    for (auto& monitorPair : folderMonitors) {
        monitorPair.second->stopRequested = true;
        
        // CORREZIONE: Chiudi handle directory per forzare uscita da ReadDirectoryChangesW
        if (monitorPair.second->directoryHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(monitorPair.second->directoryHandle);
            monitorPair.second->directoryHandle = INVALID_HANDLE_VALUE;
        }
    }
    
    // Fase 2: Attendi terminazione con timeout
    const int THREAD_TIMEOUT_MS = 3000;
    auto startTime = GetTickCount();
    
    for (auto& monitorPair : folderMonitors) {
        if (monitorPair.second->workerThread.joinable()) {
            try {
                // Calcola tempo rimanente
                DWORD elapsed = GetTickCount() - startTime;
                if (elapsed >= THREAD_TIMEOUT_MS) {
                    WriteToLog("TIMEOUT: Terminazione forzata thread " + monitorPair.first);
                    // Non fare join, lascia che il thread muoia
                    monitorPair.second->workerThread.detach();
                } else {
                    // Prova join con timeout simulato
                    bool joined = false;
                    auto joinStart = GetTickCount();
                    
                    while (GetTickCount() - joinStart < 1000 && !joined) {
                        if (monitorPair.second->workerThread.joinable()) {
                            try {
                                // Non c'è join_for in C++11, usa detach se necessario
                                if (!monitorPair.second->active) {
                                    monitorPair.second->workerThread.join();
                                    joined = true;
                                } else {
                                    Sleep(100);
                                }
                            } catch (...) {
                                WriteToLog("ERRORE join thread " + monitorPair.first);
                                monitorPair.second->workerThread.detach();
                                joined = true;
                            }
                        } else {
                            joined = true;
                        }
                    }
                    
                    if (!joined) {
                        WriteToLog("TIMEOUT join: Detach forzato thread " + monitorPair.first);
                        monitorPair.second->workerThread.detach();
                    }
                }
            } catch (const std::exception& e) {
                WriteToLog("ERRORE durante join thread: " + std::string(e.what()));
                monitorPair.second->workerThread.detach();
            }
        }
    }
    
    folderMonitors.clear();
    WriteToLog("Tutti i monitor sono stati fermati");
}

void UpdateSystemMetrics() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        systemMetrics.memoryUsageMB = pmc.WorkingSetSize / (1024 * 1024);
    }
    
    systemMetrics.activeThreads = 0;
    for (const auto& monitor : folderMonitors) {
        if (monitor.second->active) {
            systemMetrics.activeThreads++;
        }
    }
    
    if (webServerRunning) systemMetrics.activeThreads++;
}

void MetricsUpdateWorker() {
    WriteToLog("Avvio thread aggiornamento metriche");
    
    while (!globalShutdown) {
        UpdateSystemMetrics();
        Sleep(METRICS_UPDATE_INTERVAL);
    }
    
    WriteToLog("Thread aggiornamento metriche terminato");
}

std::string GetSystemMetricsJson() {
    std::lock_guard<std::mutex> lock(metricsMutex);
    
    auto now = std::chrono::steady_clock::now();
    auto uptimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - systemMetrics.serviceStartTime).count();
    auto lastActivitySeconds = systemMetrics.lastFileProcessed != std::chrono::steady_clock::time_point{} ?
        std::chrono::duration_cast<std::chrono::seconds>(now - systemMetrics.lastFileProcessed).count() : -1;
    
    std::ostringstream json;
    json << "{\n";
    json << "  \"totalFilesProcessed\": " << systemMetrics.totalFilesProcessed.load() << ",\n";
    json << "  \"filesProcessedToday\": " << systemMetrics.filesProcessedToday.load() << ",\n";
    json << "  \"activeThreads\": " << systemMetrics.activeThreads.load() << ",\n";
    json << "  \"memoryUsageMB\": " << systemMetrics.memoryUsageMB.load() << ",\n";
    json << "  \"averageProcessingTime\": " << systemMetrics.averageProcessingTime.load() << ",\n";
    json << "  \"commandsExecuted\": " << systemMetrics.commandsExecuted.load() << ",\n";
    json << "  \"errorsCount\": " << systemMetrics.errorsCount.load() << ",\n";
    json << "  \"uptimeSeconds\": " << uptimeSeconds << ",\n";
    json << "  \"lastActivitySeconds\": " << lastActivitySeconds << ",\n";
    json << "  \"foldersMonitored\": " << folderMonitors.size() << ",\n";
    json << "  \"patternsConfigured\": " << patternCommandPairs.size() << ",\n";
    json << "  \"webServerRunning\": " << (webServerRunning ? "true" : "false") << ",\n";
    json << "  \"folders\": [\n";
    
    bool first = true;
    for (const auto& monitor : folderMonitors) {
        if (!first) json << ",\n";
        json << "    {\n";
        json << "      \"path\": \"" << EscapeJsonString(monitor.second->folderPath) << "\",\n";
        json << "      \"active\": " << (monitor.second->active ? "true" : "false") << ",\n";
        json << "      \"filesDetected\": " << monitor.second->filesDetected.load() << ",\n";
        json << "      \"filesProcessed\": " << monitor.second->filesProcessed.load() << "\n";
        json << "    }";
        first = false;
    }
    
    json << "\n  ],\n";
    json << "  \"patterns\": [\n";
    
    first = true;
    std::lock_guard<std::mutex> patternLock(patternStatsMutex);
    for (const auto& pattern : patternCommandPairs) {
        if (!first) json << ",\n";
        json << "    {\n";
        json << "      \"name\": \"" << EscapeJsonString(pattern.patternName) << "\",\n";
        json << "      \"folder\": \"" << EscapeJsonString(pattern.folderPath) << "\",\n";
        json << "      \"regex\": \"" << EscapeJsonString(pattern.patternRegex) << "\",\n";
        json << "      \"matchCount\": " << patternMatchCounts[pattern.patternName] << ",\n";
        json << "      \"executionCount\": " << patternExecutionCounts[pattern.patternName] << "\n";
        json << "    }";
        first = false;
    }
    
    json << "\n  ],\n";
    json << "  \"recentActivity\": [\n";
    
    first = true;
    for (const auto& activity : systemMetrics.recentActivity) {
        if (!first) json << ",\n";
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(activity.second.time_since_epoch()).count();
        json << "    {\n";
        json << "      \"message\": \"" << EscapeJsonString(activity.first) << "\",\n";
        json << "      \"timestamp\": " << timestamp << "\n";
        json << "    }";
        first = false;
    }
    
    json << "\n  ]\n";
    json << "}";
    
    return json.str();
}

std::string GetDashboardHtml() {
    return R"(<!DOCTYPE html>
<html lang="it">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PatternTriggerCommand Dashboard</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: #333;
            min-height: 100vh;
        }
        
        .container {
            max-width: 1400px;
            margin: 0 auto;
            padding: 20px;
        }
        
        .header {
            text-align: center;
            color: white;
            margin-bottom: 30px;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.3);
        }
        
        .header h1 {
            font-size: 2.5em;
            margin-bottom: 10px;
        }
        
        .header p {
            font-size: 1.1em;
            opacity: 0.9;
        }
        
        .dashboard {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        
        .card {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 15px;
            padding: 25px;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.1);
            backdrop-filter: blur(10px);
            border: 1px solid rgba(255, 255, 255, 0.2);
            transition: transform 0.3s ease, box-shadow 0.3s ease;
        }
        
        .card:hover {
            transform: translateY(-5px);
            box-shadow: 0 12px 48px rgba(0, 0, 0, 0.15);
        }
        
        .card h3 {
            color: #444;
            margin-bottom: 15px;
            font-size: 1.2em;
            border-bottom: 2px solid #667eea;
            padding-bottom: 8px;
        }
        
        .metric {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin: 10px 0;
            padding: 8px 0;
        }
        
        .metric-label {
            font-weight: 500;
            color: #666;
        }
        
        .metric-value {
            font-weight: bold;
            color: #333;
            background: linear-gradient(45deg, #667eea, #764ba2);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            background-clip: text;
        }
        
        .status {
            display: inline-block;
            width: 12px;
            height: 12px;
            border-radius: 50%;
            margin-right: 8px;
        }
        
        .status.active {
            background: #4CAF50;
            box-shadow: 0 0 10px rgba(76, 175, 80, 0.5);
        }
        
        .status.inactive {
            background: #f44336;
        }
        
        .activity-list {
            max-height: 300px;
            overflow-y: auto;
            border: 1px solid #eee;
            border-radius: 8px;
            padding: 10px;
        }
        
        .activity-item {
            padding: 8px;
            border-bottom: 1px solid #f0f0f0;
            font-size: 0.9em;
            display: flex;
            justify-content: space-between;
        }
        
        .activity-item:last-child {
            border-bottom: none;
        }
        
        .activity-time {
            color: #888;
            font-size: 0.8em;
        }
        
        .table-container {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 15px;
            padding: 25px;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.1);
            backdrop-filter: blur(10px);
            border: 1px solid rgba(255, 255, 255, 0.2);
            margin-bottom: 20px;
        }
        
        .table-container h3 {
            color: #444;
            margin-bottom: 15px;
            font-size: 1.2em;
            border-bottom: 2px solid #667eea;
            padding-bottom: 8px;
        }
        
        table {
            width: 100%;
            border-collapse: collapse;
            margin-top: 10px;
        }
        
        th, td {
            padding: 12px;
            text-align: left;
            border-bottom: 1px solid #ddd;
        }
        
        th {
            background: linear-gradient(45deg, #667eea, #764ba2);
            color: white;
            font-weight: 600;
        }
        
        tr:nth-child(even) {
            background-color: rgba(102, 126, 234, 0.05);
        }
        
        tr:hover {
            background-color: rgba(102, 126, 234, 0.1);
        }
        
        .refresh-info {
            text-align: center;
            color: white;
            margin-top: 20px;
            opacity: 0.8;
        }
        
        @media (max-width: 768px) {
            .container {
                padding: 10px;
            }
            
            .dashboard {
                grid-template-columns: 1fr;
            }
            
            .header h1 {
                font-size: 2em;
            }
            
            .card {
                padding: 15px;
            }
            
            table {
                font-size: 0.9em;
            }
            
            th, td {
                padding: 8px;
            }
        }
        
        .loading {
            text-align: center;
            color: white;
            font-size: 1.2em;
            margin: 50px 0;
        }
        
        .error {
            background: rgba(244, 67, 54, 0.1);
            border: 1px solid #f44336;
            color: #d32f2f;
            padding: 15px;
            border-radius: 8px;
            margin: 20px 0;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🎯 PatternTriggerCommand Dashboard</h1>
            <p>Monitoring Multi-Folder v3.0 - Autore: Umberto Meglio</p>
        </div>
        
        <div id="loading" class="loading">Caricamento dati...</div>
        <div id="error" class="error" style="display: none;"></div>
        
        <div id="dashboard" style="display: none;">
            <div class="dashboard">
                <div class="card">
                    <h3>📊 Statistiche Generali</h3>
                    <div class="metric">
                        <span class="metric-label">File Processati Totali</span>
                        <span class="metric-value" id="totalFiles">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">File Oggi</span>
                        <span class="metric-value" id="todayFiles">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">Comandi Eseguiti</span>
                        <span class="metric-value" id="commandsExecuted">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">Errori</span>
                        <span class="metric-value" id="errorsCount">-</span>
                    </div>
                </div>
                
                <div class="card">
                    <h3>🖥️ Sistema</h3>
                    <div class="metric">
                        <span class="metric-label">Memoria Utilizzata</span>
                        <span class="metric-value" id="memoryUsage">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">Thread Attivi</span>
                        <span class="metric-value" id="activeThreads">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">Tempo Medio Elaborazione</span>
                        <span class="metric-value" id="avgProcessing">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">Uptime</span>
                        <span class="metric-value" id="uptime">-</span>
                    </div>
                </div>
                
                <div class="card">
                    <h3>📂 Monitoraggio</h3>
                    <div class="metric">
                        <span class="metric-label">Cartelle Monitorate</span>
                        <span class="metric-value" id="foldersCount">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">Pattern Configurati</span>
                        <span class="metric-value" id="patternsCount">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">Web Server</span>
                        <span class="metric-value" id="webServerStatus">-</span>
                    </div>
                    <div class="metric">
                        <span class="metric-label">Ultima Attività</span>
                        <span class="metric-value" id="lastActivity">-</span>
                    </div>
                </div>
                
                <div class="card">
                    <h3>📋 Attività Recente</h3>
                    <div id="recentActivity" class="activity-list">
                        Caricamento...
                    </div>
                </div>
            </div>
            
            <div class="table-container">
                <h3>📁 Cartelle Monitorate</h3>
                <table id="foldersTable">
                    <thead>
                        <tr>
                            <th>Stato</th>
                            <th>Percorso</th>
                            <th>File Rilevati</th>
                            <th>File Processati</th>
                        </tr>
                    </thead>
                    <tbody id="foldersTableBody">
                    </tbody>
                </table>
            </div>
            
            <div class="table-container">
                <h3>🎯 Pattern Configurati</h3>
                <table id="patternsTable">
                    <thead>
                        <tr>
                            <th>Nome</th>
                            <th>Cartella</th>
                            <th>Regex</th>
                            <th>Match</th>
                            <th>Esecuzioni</th>
                        </tr>
                    </thead>
                    <tbody id="patternsTableBody">
                    </tbody>
                </table>
            </div>
        </div>
        
        <div class="refresh-info">
            Dashboard aggiornata automaticamente ogni 2 secondi
        </div>
    </div>
    
    <script>
        function formatUptime(seconds) {
            if (seconds < 0) return 'N/A';
            const days = Math.floor(seconds / 86400);
            const hours = Math.floor((seconds % 86400) / 3600);
            const minutes = Math.floor((seconds % 3600) / 60);
            
            if (days > 0) return `${days}d ${hours}h ${minutes}m`;
            if (hours > 0) return `${hours}h ${minutes}m`;
            return `${minutes}m`;
        }
        
        function formatLastActivity(seconds) {
            if (seconds < 0) return 'Mai';
            if (seconds < 60) return `${seconds}s fa`;
            if (seconds < 3600) return `${Math.floor(seconds / 60)}m fa`;
            if (seconds < 86400) return `${Math.floor(seconds / 3600)}h fa`;
            return `${Math.floor(seconds / 86400)}d fa`;
        }
        
        function formatTimestamp(timestamp) {
            const date = new Date(timestamp * 1000);
            return date.toLocaleTimeString();
        }
        
        function updateDashboard() {
            fetch('/api/metrics')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('loading').style.display = 'none';
                    document.getElementById('error').style.display = 'none';
                    document.getElementById('dashboard').style.display = 'block';
                    
                    // Aggiorna metriche generali
                    document.getElementById('totalFiles').textContent = data.totalFilesProcessed.toLocaleString();
                    document.getElementById('todayFiles').textContent = data.filesProcessedToday.toLocaleString();
                    document.getElementById('commandsExecuted').textContent = data.commandsExecuted.toLocaleString();
                    document.getElementById('errorsCount').textContent = data.errorsCount.toLocaleString();
                    
                    // Aggiorna metriche sistema
                    document.getElementById('memoryUsage').textContent = data.memoryUsageMB + ' MB';
                    document.getElementById('activeThreads').textContent = data.activeThreads;
                    document.getElementById('avgProcessing').textContent = data.averageProcessingTime + ' ms';
                    document.getElementById('uptime').textContent = formatUptime(data.uptimeSeconds);
                    
                    // Aggiorna monitoraggio
                    document.getElementById('foldersCount').textContent = data.foldersMonitored;
                    document.getElementById('patternsCount').textContent = data.patternsConfigured;
                    document.getElementById('webServerStatus').innerHTML = data.webServerRunning ? 
                        '<span class="status active"></span>Attivo' : 
                        '<span class="status inactive"></span>Inattivo';
                    document.getElementById('lastActivity').textContent = formatLastActivity(data.lastActivitySeconds);
                    
                    // Aggiorna tabella cartelle
                    const foldersBody = document.getElementById('foldersTableBody');
                    foldersBody.innerHTML = '';
                    data.folders.forEach(folder => {
                        const row = foldersBody.insertRow();
                        row.innerHTML = `
                            <td><span class="status ${folder.active ? 'active' : 'inactive'}"></span>${folder.active ? 'Attivo' : 'Inattivo'}</td>
                            <td>${folder.path}</td>
                            <td>${folder.filesDetected}</td>
                            <td>${folder.filesProcessed}</td>
                        `;
                    });
                    
                    // Aggiorna tabella pattern
                    const patternsBody = document.getElementById('patternsTableBody');
                    patternsBody.innerHTML = '';
                    data.patterns.forEach(pattern => {
                        const row = patternsBody.insertRow();
                        row.innerHTML = `
                            <td>${pattern.name}</td>
                            <td>${pattern.folder}</td>
                            <td><code>${pattern.regex}</code></td>
                            <td>${pattern.matchCount}</td>
                            <td>${pattern.executionCount}</td>
                        `;
                    });
                    
                    // Aggiorna attività recente
                    const activityDiv = document.getElementById('recentActivity');
                    activityDiv.innerHTML = '';
                    data.recentActivity.forEach(activity => {
                        const div = document.createElement('div');
                        div.className = 'activity-item';
                        div.innerHTML = `
                            <span>${activity.message}</span>
                            <span class="activity-time">${formatTimestamp(activity.timestamp)}</span>
                        `;
                        activityDiv.appendChild(div);
                    });
                })
                .catch(error => {
                    document.getElementById('loading').style.display = 'none';
                    document.getElementById('dashboard').style.display = 'none';
                    const errorDiv = document.getElementById('error');
                    errorDiv.textContent = 'Errore di connessione: ' + error.message;
                    errorDiv.style.display = 'block';
                });
        }
        
        // Aggiorna immediatamente e poi ogni 2 secondi
        updateDashboard();
        setInterval(updateDashboard, 2000);
    </script>
</body>
</html>)";
}

std::string HandleHttpRequest(const std::string& request) {
    std::string response;
    
    if (request.find("GET / ") != std::string::npos || request.find("GET /dashboard") != std::string::npos) {
        std::string html = GetDashboardHtml();
        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: text/html; charset=utf-8\r\n";
        response += "Content-Length: " + std::to_string(html.length()) + "\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "\r\n";
        response += html;
    }
    else if (request.find("GET /api/metrics") != std::string::npos) {
        std::string json = GetSystemMetricsJson();
        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(json.length()) + "\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "\r\n";
        response += json;
    }
    else {
        std::string notFound = "404 Not Found";
        response = "HTTP/1.1 404 Not Found\r\n";
        response += "Content-Type: text/plain\r\n";
        response += "Content-Length: " + std::to_string(notFound.length()) + "\r\n";
        response += "\r\n";
        response += notFound;
    }
    
    return response;
}

void WebServerWorker() {
    WriteToLog("Avvio web server sulla porta " + std::to_string(webServerPort));
    
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        WriteToLog("ERRORE: WSAStartup fallito");
        systemMetrics.errorsCount++;
        return;
    }
    
    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        WriteToLog("ERRORE: Creazione socket fallita");
        systemMetrics.errorsCount++;
        WSACleanup();
        return;
    }
    
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(webServerPort);
    
    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        WriteToLog("ERRORE: Bind socket fallito sulla porta " + std::to_string(webServerPort));
        systemMetrics.errorsCount++;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    
    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        WriteToLog("ERRORE: Listen socket fallito");
        systemMetrics.errorsCount++;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    
    // CORREZIONE: Timeout più aggressivo e non-bloccante
    DWORD timeout = 500; // 500ms invece di 1 secondo
    setsockopt(serverSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    
    // Imposta socket non-bloccante
    u_long mode = 1;
    ioctlsocket(serverSocket, FIONBIO, &mode);
    
    webServerRunning = true;
    WriteToLog("Web server avviato su http://localhost:" + std::to_string(webServerPort));
    
    while (!globalShutdown && !webServerShouldStop) {
        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        
        if (clientSocket == INVALID_SOCKET) {
            DWORD error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAETIMEDOUT) {
                // CORREZIONE: Check più frequenti
                Sleep(50);
                continue;
            } else {
                WriteToLog("ERRORE: Accept fallito: " + std::to_string(error));
                if (!globalShutdown && !webServerShouldStop) {
                    systemMetrics.errorsCount++;
                }
                break;
            }
        }
        
        if (globalShutdown || webServerShouldStop) {
            closesocket(clientSocket);
            break;
        }
        
        // CORREZIONE: Timeout più aggressivo anche per recv
        timeout = 1000;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        
        char buffer[4096];
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            std::string request(buffer);
            std::string response = HandleHttpRequest(request);
            
            // CORREZIONE: Timeout anche per send
            timeout = 1000;
            setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
            send(clientSocket, response.c_str(), response.length(), 0);
        }
        
        closesocket(clientSocket);
        
        // CORREZIONE: Check più frequenti
        if (globalShutdown || webServerShouldStop) break;
    }
    
    webServerRunning = false;
    closesocket(serverSocket);
    WSACleanup();
    WriteToLog("Web server terminato");
}

DWORD WINAPI ServiceWorkerThread(LPVOID /*lpParam*/) {
    WriteToLog("=== Servizio PatternTriggerCommand Multi-Folder v3.0 avviato ===");
    
    if (!LoadConfiguration()) {
        WriteToLog("ERRORE FATALE: Impossibile caricare configurazione");
        return 1;
    }
    
    std::string scriptsDir = "C:\\Scripts";
    CreateDirectoryRecursive(scriptsDir);
    
    LoadProcessedFiles();
    
    recentlyIgnoredFiles.clear();
    
    // Avvia thread aggiornamento metriche
    std::thread metricsThread(MetricsUpdateWorker);
    
    // Avvia web server se abilitato
    if (webServerEnabled) {
        webServerThread = std::thread(WebServerWorker);
    }
    
    StartAllFolderMonitors();
    
    WriteToLog("Servizio in esecuzione, attesa terminazione...");
    if (webServerEnabled) {
        WriteToLog("Dashboard disponibile su: http://localhost:" + std::to_string(webServerPort));
    }
    
    DWORD lastCleanup = GetTickCount();
    
    while (!globalShutdown) {
        if (WaitForSingleObject(stopEvent, 5000) == WAIT_OBJECT_0) {
            break;
        }
        
        int activeMonitors = 0;
        for (const auto& monitorPair : folderMonitors) {
            if (monitorPair.second->active) activeMonitors++;
        }
        WriteToLog("Monitor attivi: " + std::to_string(activeMonitors), true);
        
        if (GetTickCount() - lastCleanup > CACHE_CLEANUP_INTERVAL) {
            recentlyIgnoredFiles.clear();
            lastCleanup = GetTickCount();
            WriteToLog("Pulizia cache file ignorati", true);
        }
    }
    
    WriteToLog("Terminazione richiesta, cleanup in corso...");
    
    // CORREZIONE: Arresto MOLTO più aggressivo con timeout brevi
    
    // 1. Ferma web server per primo (TIMEOUT: 2 secondi)
    if (webServerEnabled) {
        webServerShouldStop = true;
        WriteToLog("Arresto web server...");
        
        auto webStartTime = GetTickCount();
        bool webStopped = false;
        
        while (GetTickCount() - webStartTime < 2000 && !webStopped) {
            if (!webServerRunning) {
                webStopped = true;
            } else {
                Sleep(100);
            }
        }
        
        if (webServerThread.joinable()) {
            if (webStopped) {
                try {
                    webServerThread.join();
                    WriteToLog("Web server arrestato correttamente");
                } catch (...) {
                    WriteToLog("ERRORE join web server - detach forzato");
                    webServerThread.detach();
                }
            } else {
                WriteToLog("TIMEOUT web server - detach forzato");
                webServerThread.detach();
            }
        }
    }
    
    // 2. Ferma monitor cartelle (TIMEOUT: 3 secondi)
    WriteToLog("Arresto monitor cartelle...");
    StopAllFolderMonitors();
    
    // 3. Ferma thread metriche (TIMEOUT: 1 secondo)
    if (metricsThread.joinable()) {
        WriteToLog("Arresto thread metriche...");
        
        auto metricsStartTime = GetTickCount();
        bool metricsJoined = false;
        
        while (GetTickCount() - metricsStartTime < 1000 && !metricsJoined) {
            try {
                // Simula join con timeout
                if (metricsThread.joinable()) {
                    // Non c'è join_for in C++11, uso detach se necessario
                    Sleep(100);
                } else {
                    metricsJoined = true;
                }
            } catch (...) {
                break;
            }
        }
        
        try {
            if (metricsThread.joinable()) {
                metricsThread.join();
                WriteToLog("Thread metriche arrestato");
            }
        } catch (...) {
            WriteToLog("TIMEOUT metriche - detach forzato");
            metricsThread.detach();
        }
    }
    
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
            serviceStatus.dwCheckPoint = 1;
            serviceStatus.dwWaitHint = 5000; // CORREZIONE: Ridotto da 8 a 5 secondi
            
            SetServiceStatus(serviceStatusHandle, &serviceStatus);
            
            // CORREZIONE: Arresto immediato e aggressivo
            globalShutdown = true;
            webServerShouldStop = true;
            
            if (stopEvent) {
                SetEvent(stopEvent);
            }
            
            // CORREZIONE: Forza chiusura handle directory per sbloccare ReadDirectoryChangesW
            for (auto& monitorPair : folderMonitors) {
                if (monitorPair.second->directoryHandle != INVALID_HANDLE_VALUE) {
                    CloseHandle(monitorPair.second->directoryHandle);
                    monitorPair.second->directoryHandle = INVALID_HANDLE_VALUE;
                }
            }
            
            break;
            
        default:
            break;
    }
}

void WINAPI ServiceMain(DWORD /*argc*/, LPTSTR* /*argv*/) {
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
    
    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stopEvent == NULL) {
        WriteToLog("CreateEvent fallito");
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        serviceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(serviceStatusHandle, &serviceStatus);
        return;
    }
    
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
    
    WriteToLog("Servizio avviato e registrato");
    
    WaitForSingleObject(hThread, INFINITE);
    
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
        globalShutdown = true;
        webServerShouldStop = true;
        if (stopEvent) {
            SetEvent(stopEvent);
        }
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    std::string configFileStr = DEFAULT_CONFIG_FILE;
    std::string baseDir = configFileStr.substr(0, configFileStr.find_last_of("\\/"));
    CreateDirectoryRecursive(baseDir);
    
    WriteToLog("=== PatternTriggerCommand Multi-Folder v3.0 + Dashboard ===");
    WriteToLog("Autore: Umberto Meglio - Supporto: Claude di Anthropic");
    
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
            if (webServerEnabled) {
                std::cout << "Web server abilitato sulla porta " << webServerPort << std::endl;
                std::cout << "Dashboard: http://localhost:" << webServerPort << std::endl;
            }
            
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
            
            std::cout << "=== Status PatternTriggerCommand v3.0 ===" << std::endl;
            std::cout << "File processati: " << processedFiles.size() << std::endl;
            
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
                            case SERVICE_RUNNING: 
                                std::cout << "IN ESECUZIONE";
                                if (webServerEnabled) {
                                    std::cout << " - Dashboard: http://localhost:" << webServerPort;
                                }
                                break;
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
            
            std::map<std::string, int> folderCounts;
            for (const auto& pair : patternCommandPairs) {
                folderCounts[pair.folderPath]++;
            }
            
            std::cout << "Cartelle monitorate: " << folderCounts.size() << std::endl;
            for (const auto& folder : folderCounts) {
                std::cout << "  " << folder.first << " (" << folder.second << " pattern/s)" << std::endl;
            }
            
            if (webServerEnabled) {
                std::cout << "Web server: Abilitato sulla porta " << webServerPort << std::endl;
            } else {
                std::cout << "Web server: Disabilitato" << std::endl;
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
            if (webServerEnabled) {
                std::cout << "Web server configurato sulla porta " << webServerPort << std::endl;
            }
        }
        else if (command == "reprocess" && argc > 3) {
            LoadConfiguration();
            std::string folderPath = argv[2];
            std::string filename = argv[3];
            std::string fullPath = folderPath + "\\" + filename;
            
            if (FileExists(fullPath)) {
                LoadProcessedFiles();
                
                {
                    std::lock_guard<std::mutex> lock(processedFilesMutex);
                    processedFiles.erase(fullPath);
                }
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
        WriteToLog("Avvio come servizio Windows");
        
        SERVICE_TABLE_ENTRY serviceTable[] = {
            { const_cast<LPSTR>(SERVICE_NAME), (LPSERVICE_MAIN_FUNCTION)ServiceMain },
            { NULL, NULL }
        };
        
        if (StartServiceCtrlDispatcher(serviceTable) == FALSE) {
            std::cerr << "Errore: Eseguire come servizio Windows o con parametri." << std::endl;
            std::cerr << "Per aiuto: PatternTriggerCommand.exe status" << std::endl;
            return 1;
        }
    }
    
    return 0;
}
