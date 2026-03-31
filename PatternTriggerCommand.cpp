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
#define SCHEDULER_CHECK_INTERVAL 15000
#define DEFAULT_SCHEDULER_FOLDER "C:\\PTC\\schedules"

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
std::mutex schedulerMutex;

// Configurazione
std::string defaultMonitoredFolder = DEFAULT_MONITORED_FOLDER;
std::string configFile = DEFAULT_CONFIG_FILE;
std::string logFile = DEFAULT_LOG_FILE;
std::string detailedLogFile = DEFAULT_DETAILED_LOG_FILE;
std::string processedFilesDb = DEFAULT_PROCESSED_FILES_DB;
bool detailedLogging = true;
int webServerPort = DEFAULT_WEB_PORT;
bool webServerEnabled = true;
std::string schedulerFolder = DEFAULT_SCHEDULER_FOLDER;
bool schedulerEnabled = true;

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

// Schedulatore
struct SchedulerTask {
    std::string name;
    bool enabled;
    std::set<int> days;      // 0=Do, 1=Lu, 2=Ma, 3=Me, 4=Gi, 5=Ve, 6=Sa
    std::set<int> hours;     // 0-23
    std::set<int> minutes;   // 0-59
    std::string command;
    int intervalSeconds;     // 0 = usa trigger giorno/ora/minuto, >0 = ripeti ogni N secondi
    int lastFiredMinute;
    int lastFiredHour;
    int lastFiredDayOfYear;
    std::string lastExecutionTime;
    size_t executionCount;
    std::chrono::steady_clock::time_point lastIntervalRun;

    SchedulerTask() : enabled(true), intervalSeconds(0), lastFiredMinute(-1), lastFiredHour(-1),
                      lastFiredDayOfYear(-1), executionCount(0) {}
};

struct SchedulerExecution {
    std::string taskName;
    std::string timestamp;
    std::string command;
    int exitCode;
    bool success;
};

std::vector<SchedulerTask> schedulerTasks;
std::vector<SchedulerExecution> schedulerHistory;
#define MAX_SCHEDULER_HISTORY 200
std::thread schedulerThread;

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

// Scheduler
std::string SanitizeFilename(const std::string& name);
int DayNameToNumber(const std::string& dayName);
std::string DayNumberToName(int dayNum);
bool LoadSchedulerTasks();
bool SaveSchedulerTask(const SchedulerTask& task);
bool DeleteSchedulerTask(const std::string& name);
void SchedulerWorker();
void RecordSchedulerExecution(const std::string& taskName, const std::string& command, int exitCode, bool success);
std::string GetSchedulerJson();
std::string GetSchedulerScriptsJson();
std::string GetSchedulerPageHtml();
std::string ExtractJsonValue(const std::string& json, const std::string& key);
std::string GetHttpRequestBody(const std::string& request);

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
            config << "WebServerEnabled=" << (webServerEnabled ? "true" : "false") << "\n";
            config << "SchedulerEnabled=" << (schedulerEnabled ? "true" : "false") << "\n";
            config << "SchedulerFolder=" << schedulerFolder << "\n\n";
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
            } else if (key == "SchedulerEnabled") {
                schedulerEnabled = (value == "true" || value == "1" || value == "yes");
            } else if (key == "SchedulerFolder") {
                schedulerFolder = value;
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
    json << "  \"schedulerEnabled\": " << (schedulerEnabled ? "true" : "false") << ",\n";
    json << "  \"schedulerTasks\": " << schedulerTasks.size() << ",\n";
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

// ====== IMPLEMENTAZIONE SCHEDULATORE ======

std::string SanitizeFilename(const std::string& name) {
    std::string safe;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
            safe += c;
        } else if (c == ' ') {
            safe += '_';
        }
    }
    return safe.empty() ? "unnamed" : safe;
}

int DayNameToNumber(const std::string& dayName) {
    if (dayName == "Do" || dayName == "do" || dayName == "DO") return 0;
    if (dayName == "Lu" || dayName == "lu" || dayName == "LU") return 1;
    if (dayName == "Ma" || dayName == "ma" || dayName == "MA") return 2;
    if (dayName == "Me" || dayName == "me" || dayName == "ME") return 3;
    if (dayName == "Gi" || dayName == "gi" || dayName == "GI") return 4;
    if (dayName == "Ve" || dayName == "ve" || dayName == "VE") return 5;
    if (dayName == "Sa" || dayName == "sa" || dayName == "SA") return 6;
    return -1;
}

std::string DayNumberToName(int dayNum) {
    switch (dayNum) {
        case 0: return "Do";
        case 1: return "Lu";
        case 2: return "Ma";
        case 3: return "Me";
        case 4: return "Gi";
        case 5: return "Ve";
        case 6: return "Sa";
        default: return "?";
    }
}

std::string GetHttpRequestBody(const std::string& request) {
    size_t bodyStart = request.find("\r\n\r\n");
    if (bodyStart == std::string::npos) return "";
    return request.substr(bodyStart + 4);
}

std::string ExtractJsonValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";

    size_t valueStart = json.find_first_not_of(" \t\n\r", colonPos + 1);
    if (valueStart == std::string::npos) return "";

    if (json[valueStart] == '"') {
        std::string result;
        for (size_t i = valueStart + 1; i < json.length(); ++i) {
            if (json[i] == '\\' && i + 1 < json.length()) {
                result += json[i + 1];
                ++i;
            } else if (json[i] == '"') {
                break;
            } else {
                result += json[i];
            }
        }
        return result;
    } else {
        size_t valueEnd = json.find_first_of(",} \t\n\r", valueStart);
        if (valueEnd == std::string::npos) return json.substr(valueStart);
        return json.substr(valueStart, valueEnd - valueStart);
    }
}

bool LoadSchedulerTasks() {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    schedulerTasks.clear();

    if (!DirectoryExists(schedulerFolder)) {
        CreateDirectoryRecursive(schedulerFolder);
        WriteToLog("Creata cartella schedulatore: " + schedulerFolder);
        return true;
    }

    WIN32_FIND_DATA findData;
    std::string searchPath = schedulerFolder + "\\*.sch";
    HANDLE hFind = FindFirstFile(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        WriteToLog("Nessun task schedulato trovato in: " + schedulerFolder);
        return true;
    }

    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::string filePath = schedulerFolder + "\\" + findData.cFileName;
        std::ifstream file(filePath.c_str());
        if (!file.is_open()) continue;

        SchedulerTask task;
        std::string line;

        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            size_t eqPos = line.find('=');
            if (eqPos == std::string::npos) continue;

            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (key == "Name") {
                task.name = value;
            } else if (key == "Enabled") {
                task.enabled = (value == "true" || value == "1");
            } else if (key == "Days") {
                std::istringstream ss(value);
                std::string day;
                while (std::getline(ss, day, ',')) {
                    day.erase(0, day.find_first_not_of(" \t"));
                    day.erase(day.find_last_not_of(" \t") + 1);
                    int dayNum = DayNameToNumber(day);
                    if (dayNum >= 0) task.days.insert(dayNum);
                }
            } else if (key == "Hours") {
                std::istringstream ss(value);
                std::string hour;
                while (std::getline(ss, hour, ',')) {
                    hour.erase(0, hour.find_first_not_of(" \t"));
                    hour.erase(hour.find_last_not_of(" \t") + 1);
                    try {
                        int h = std::stoi(hour);
                        if (h >= 0 && h <= 23) task.hours.insert(h);
                    } catch (...) {}
                }
            } else if (key == "Minutes") {
                std::istringstream ss(value);
                std::string minute;
                while (std::getline(ss, minute, ',')) {
                    minute.erase(0, minute.find_first_not_of(" \t"));
                    minute.erase(minute.find_last_not_of(" \t") + 1);
                    try {
                        int m = std::stoi(minute);
                        if (m >= 0 && m <= 59) task.minutes.insert(m);
                    } catch (...) {}
                }
            } else if (key == "Command") {
                task.command = value;
            } else if (key == "Interval") {
                try { task.intervalSeconds = std::stoi(value); } catch (...) { task.intervalSeconds = 0; }
            }
        }

        file.close();

        if (!task.name.empty() && !task.command.empty()) {
            task.lastIntervalRun = std::chrono::steady_clock::now();
            schedulerTasks.push_back(task);
            WriteToLog("Task schedulato caricato: " + task.name, true);
        }
    } while (FindNextFile(hFind, &findData));

    FindClose(hFind);

    WriteToLog("Task schedulati caricati: " + std::to_string(schedulerTasks.size()));
    return true;
}

bool SaveSchedulerTask(const SchedulerTask& task) {
    if (task.name.empty()) return false;

    if (!DirectoryExists(schedulerFolder)) {
        CreateDirectoryRecursive(schedulerFolder);
    }

    std::string filename = SanitizeFilename(task.name) + ".sch";
    std::string filePath = schedulerFolder + "\\" + filename;

    std::ofstream file(filePath.c_str());
    if (!file.is_open()) {
        WriteToLog("ERRORE: Impossibile salvare task schedulato: " + filePath);
        return false;
    }

    file << "# PatternTriggerCommand - Task Schedulato\n";
    file << "Name=" << task.name << "\n";
    file << "Enabled=" << (task.enabled ? "true" : "false") << "\n";

    file << "Days=";
    bool first = true;
    for (int d : task.days) {
        if (!first) file << ",";
        file << DayNumberToName(d);
        first = false;
    }
    file << "\n";

    file << "Hours=";
    first = true;
    for (int h : task.hours) {
        if (!first) file << ",";
        file << h;
        first = false;
    }
    file << "\n";

    file << "Minutes=";
    first = true;
    for (int m : task.minutes) {
        if (!first) file << ",";
        file << m;
        first = false;
    }
    file << "\n";

    file << "Command=" << task.command << "\n";
    file << "Interval=" << task.intervalSeconds << "\n";
    file.close();

    WriteToLog("Task schedulato salvato: " + task.name);
    return true;
}

bool DeleteSchedulerTask(const std::string& name) {
    // Trova e rimuovi il file .sch corrispondente
    std::string filename = SanitizeFilename(name) + ".sch";
    std::string filePath = schedulerFolder + "\\" + filename;

    if (DeleteFile(filePath.c_str())) {
        std::lock_guard<std::mutex> lock(schedulerMutex);
        schedulerTasks.erase(
            std::remove_if(schedulerTasks.begin(), schedulerTasks.end(),
                [&name](const SchedulerTask& t) { return t.name == name; }),
            schedulerTasks.end()
        );
        WriteToLog("Task schedulato eliminato: " + name);
        return true;
    }

    WriteToLog("ERRORE: Impossibile eliminare task: " + name);
    return false;
}

void RecordSchedulerExecution(const std::string& taskName, const std::string& command, int exitCode, bool success) {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    SchedulerExecution exec;
    exec.taskName = taskName;
    exec.timestamp = GetTimestamp();
    exec.command = command;
    exec.exitCode = exitCode;
    exec.success = success;
    schedulerHistory.push_back(exec);
    if (schedulerHistory.size() > MAX_SCHEDULER_HISTORY) {
        schedulerHistory.erase(schedulerHistory.begin());
    }
}

void SchedulerExecuteTask(const std::string& cmd, const std::string& taskName) {
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdLine = "cmd.exe /C \"" + cmd + "\"";

    if (CreateProcess(NULL, const_cast<LPSTR>(cmdLine.c_str()),
                     NULL, NULL, FALSE, CREATE_NO_WINDOW,
                     NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, BATCH_TIMEOUT);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        WriteToLog("Schedulatore: Task '" + taskName +
                 "' completato con codice: " + std::to_string(exitCode));
        RecordSchedulerExecution(taskName, cmd, static_cast<int>(exitCode), exitCode == 0);
    } else {
        DWORD err = GetLastError();
        WriteToLog("ERRORE Schedulatore: Impossibile eseguire task '" +
                 taskName + "': " + std::to_string(err));
        RecordSchedulerExecution(taskName, cmd, static_cast<int>(err), false);
    }
}

void SchedulerWorker() {
    WriteToLog("Avvio thread schedulatore");

    while (!globalShutdown) {
        if (!schedulerEnabled) {
            Sleep(SCHEDULER_CHECK_INTERVAL);
            continue;
        }

        SYSTEMTIME st;
        GetLocalTime(&st);

        int currentDay = st.wDayOfWeek;
        int currentHour = st.wHour;
        int currentMinute = st.wMinute;
        int currentDayOfYear = st.wDay + st.wMonth * 31;
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(schedulerMutex);
            for (auto& task : schedulerTasks) {
                if (!task.enabled) continue;

                bool shouldFire = false;

                if (task.intervalSeconds > 0) {
                    // Modalita' intervallo: ripeti ogni N secondi
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - task.lastIntervalRun).count();
                    if (elapsed >= task.intervalSeconds) {
                        shouldFire = true;
                        task.lastIntervalRun = now;
                    }
                } else {
                    // Modalita' trigger giorno/ora/minuto
                    if (task.lastFiredDayOfYear == currentDayOfYear &&
                        task.lastFiredHour == currentHour &&
                        task.lastFiredMinute == currentMinute) {
                        continue;
                    }

                    if (task.days.count(currentDay) &&
                        task.hours.count(currentHour) &&
                        task.minutes.count(currentMinute)) {
                        shouldFire = true;
                        task.lastFiredDayOfYear = currentDayOfYear;
                        task.lastFiredHour = currentHour;
                        task.lastFiredMinute = currentMinute;
                    }
                }

                if (shouldFire) {
                    WriteToLog("Schedulatore: Esecuzione task '" + task.name + "' - Comando: " + task.command);
                    task.lastExecutionTime = GetTimestamp();
                    task.executionCount++;

                    std::string cmd = task.command;
                    std::string taskName = task.name;
                    std::thread(SchedulerExecuteTask, cmd, taskName).detach();
                }
            }
        }

        Sleep(SCHEDULER_CHECK_INTERVAL);
    }

    WriteToLog("Thread schedulatore terminato");
}

std::string GetSchedulerJson() {
    std::lock_guard<std::mutex> lock(schedulerMutex);

    std::ostringstream json;
    json << "{\n";
    json << "  \"enabled\": " << (schedulerEnabled ? "true" : "false") << ",\n";
    json << "  \"folder\": \"" << EscapeJsonString(schedulerFolder) << "\",\n";
    json << "  \"tasks\": [\n";

    bool first = true;
    for (const auto& task : schedulerTasks) {
        if (!first) json << ",\n";
        json << "    {\n";
        json << "      \"name\": \"" << EscapeJsonString(task.name) << "\",\n";
        json << "      \"enabled\": " << (task.enabled ? "true" : "false") << ",\n";
        json << "      \"intervalSeconds\": " << task.intervalSeconds << ",\n";

        json << "      \"days\": \"";
        bool innerFirst = true;
        for (int d : task.days) {
            if (!innerFirst) json << ",";
            json << DayNumberToName(d);
            innerFirst = false;
        }
        json << "\",\n";

        json << "      \"hours\": \"";
        innerFirst = true;
        for (int h : task.hours) {
            if (!innerFirst) json << ",";
            json << h;
            innerFirst = false;
        }
        json << "\",\n";

        json << "      \"minutes\": \"";
        innerFirst = true;
        for (int m : task.minutes) {
            if (!innerFirst) json << ",";
            json << m;
            innerFirst = false;
        }
        json << "\",\n";

        json << "      \"command\": \"" << EscapeJsonString(task.command) << "\",\n";
        json << "      \"lastExecution\": \"" << EscapeJsonString(task.lastExecutionTime) << "\",\n";
        json << "      \"executionCount\": " << task.executionCount << "\n";
        json << "    }";
        first = false;
    }

    json << "\n  ],\n";
    json << "  \"history\": [\n";

    first = true;
    for (int i = static_cast<int>(schedulerHistory.size()) - 1; i >= 0; --i) {
        const auto& exec = schedulerHistory[i];
        if (!first) json << ",\n";
        json << "    {\n";
        json << "      \"taskName\": \"" << EscapeJsonString(exec.taskName) << "\",\n";
        json << "      \"timestamp\": \"" << EscapeJsonString(exec.timestamp) << "\",\n";
        json << "      \"command\": \"" << EscapeJsonString(exec.command) << "\",\n";
        json << "      \"exitCode\": " << exec.exitCode << ",\n";
        json << "      \"success\": " << (exec.success ? "true" : "false") << "\n";
        json << "    }";
        first = false;
    }

    json << "\n  ]\n";
    json << "}";

    return json.str();
}

std::string GetSchedulerScriptsJson() {
    std::ostringstream json;
    json << "[";

    std::vector<std::string> searchDirs = {"C:\\Scripts", schedulerFolder};
    bool first = true;

    for (const auto& dir : searchDirs) {
        if (!DirectoryExists(dir)) continue;

        std::string patterns[] = {"\\*.bat", "\\*.cmd", "\\*.exe", "\\*.ps1"};
        for (const auto& ext : patterns) {
            WIN32_FIND_DATA fd;
            HANDLE hFind = FindFirstFile((dir + ext).c_str(), &fd);
            if (hFind == INVALID_HANDLE_VALUE) continue;

            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!first) json << ",";
                json << "\"" << EscapeJsonString(dir + "\\" + fd.cFileName) << "\"";
                first = false;
            } while (FindNextFile(hFind, &fd));

            FindClose(hFind);
        }
    }

    json << "]";
    return json.str();
}

std::string GetSchedulerPageHtml() {
    return R"html(<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>PTC - Schedulatore</title>
<style>
:root{--bg:#0f172a;--surface:#1e293b;--surface2:#334155;--border:#475569;--primary:#6366f1;--primary-light:#818cf8;--accent:#22d3ee;--success:#22c55e;--warning:#f59e0b;--danger:#ef4444;--text:#f1f5f9;--text2:#94a3b8;--text3:#64748b;--radius:10px;}
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:'Segoe UI',system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);min-height:100vh;}
.top-bar{background:var(--surface);border-bottom:1px solid var(--border);padding:12px 24px;display:flex;align-items:center;justify-content:space-between;position:sticky;top:0;z-index:50;backdrop-filter:blur(12px);}
.top-bar h1{font-size:1.3em;font-weight:700;background:linear-gradient(135deg,var(--primary-light),var(--accent));-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.top-bar a{color:var(--text2);text-decoration:none;font-size:0.9em;padding:6px 16px;border:1px solid var(--border);border-radius:20px;transition:all 0.2s;}
.top-bar a:hover{color:var(--text);border-color:var(--primary);}
.main{max-width:1440px;margin:0 auto;padding:20px;}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin-bottom:24px;}
.stat-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:16px 20px;transition:border-color 0.2s;}
.stat-card:hover{border-color:var(--primary);}
.stat-label{font-size:0.78em;color:var(--text3);text-transform:uppercase;letter-spacing:0.5px;margin-bottom:4px;}
.stat-value{font-size:1.5em;font-weight:700;color:var(--text);}
.stat-value.on{color:var(--success);}
.stat-value.off{color:var(--danger);}
.tabs{display:flex;gap:4px;margin-bottom:20px;background:var(--surface);border-radius:var(--radius);padding:4px;border:1px solid var(--border);}
.tab{padding:10px 24px;border-radius:8px;cursor:pointer;font-weight:600;font-size:0.9em;color:var(--text2);transition:all 0.2s;border:none;background:transparent;}
.tab:hover{color:var(--text);}
.tab.active{background:var(--primary);color:white;}
.panel{display:none;}
.panel.active{display:block;}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:20px;margin-bottom:16px;}
.card-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px;}
.card-title{font-size:1.1em;font-weight:700;color:var(--text);}
.btn{padding:8px 18px;border:none;border-radius:8px;cursor:pointer;font-size:0.85em;font-weight:600;transition:all 0.15s;display:inline-flex;align-items:center;gap:6px;}
.btn:hover{transform:translateY(-1px);filter:brightness(1.1);}
.btn:active{transform:translateY(0);}
.btn-primary{background:var(--primary);color:white;}
.btn-success{background:var(--success);color:white;}
.btn-warning{background:var(--warning);color:#000;}
.btn-danger{background:var(--danger);color:white;}
.btn-ghost{background:transparent;color:var(--text2);border:1px solid var(--border);}
.btn-ghost:hover{color:var(--text);border-color:var(--text2);}
.btn-sm{padding:5px 12px;font-size:0.8em;}
table{width:100%;border-collapse:collapse;}
th{text-align:left;padding:10px 14px;font-size:0.78em;color:var(--text3);text-transform:uppercase;letter-spacing:0.5px;border-bottom:1px solid var(--border);font-weight:600;}
td{padding:10px 14px;border-bottom:1px solid rgba(71,85,105,0.3);font-size:0.9em;vertical-align:middle;}
tr:hover td{background:rgba(99,102,241,0.04);}
.badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:0.78em;font-weight:600;}
.badge-on{background:rgba(34,197,94,0.15);color:var(--success);}
.badge-off{background:rgba(239,68,68,0.15);color:var(--danger);}
.badge-interval{background:rgba(34,211,238,0.15);color:var(--accent);}
.badge-schedule{background:rgba(129,140,248,0.15);color:var(--primary-light);}
.mono{font-family:'Cascadia Code','Fira Code',monospace;font-size:0.85em;color:var(--accent);background:rgba(34,211,238,0.08);padding:2px 8px;border-radius:4px;}
.actions{display:flex;gap:4px;flex-wrap:wrap;}
.overlay{position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);display:flex;align-items:center;justify-content:center;z-index:100;backdrop-filter:blur(4px);}
.modal{background:var(--surface);border:1px solid var(--border);border-radius:14px;padding:28px;width:92%;max-width:640px;max-height:90vh;overflow-y:auto;}
.modal h2{font-size:1.2em;margin-bottom:20px;color:var(--text);padding-bottom:12px;border-bottom:1px solid var(--border);}
.field{margin-bottom:18px;}
.field label{display:block;font-size:0.85em;font-weight:600;color:var(--text2);margin-bottom:6px;}
.field input[type=text],.field input[type=number],.field select{width:100%;padding:10px 14px;background:var(--bg);border:1px solid var(--border);border-radius:8px;color:var(--text);font-size:0.95em;transition:border-color 0.2s;}
.field input:focus,.field select:focus{outline:none;border-color:var(--primary);}
.field select{cursor:pointer;}
.field .hint{font-size:0.78em;color:var(--text3);margin-top:4px;}
.days-row{display:flex;gap:6px;flex-wrap:wrap;}
.day-chip{padding:8px 14px;border-radius:8px;cursor:pointer;font-weight:600;font-size:0.9em;background:var(--bg);border:2px solid var(--border);color:var(--text2);transition:all 0.15s;user-select:none;}
.day-chip.on{background:rgba(99,102,241,0.2);border-color:var(--primary);color:var(--primary-light);}
.mode-switch{display:flex;gap:4px;background:var(--bg);border-radius:8px;padding:4px;margin-bottom:12px;}
.mode-btn{flex:1;padding:10px;border:none;border-radius:6px;cursor:pointer;font-weight:600;font-size:0.88em;color:var(--text2);background:transparent;transition:all 0.2s;}
.mode-btn.active{background:var(--primary);color:white;}
.mode-section{display:none;}
.mode-section.active{display:block;}
.modal-actions{display:flex;gap:10px;justify-content:flex-end;margin-top:24px;padding-top:16px;border-top:1px solid var(--border);}
.history-filters{display:flex;gap:10px;margin-bottom:14px;flex-wrap:wrap;}
.history-filters input,.history-filters select{padding:8px 12px;background:var(--bg);border:1px solid var(--border);border-radius:8px;color:var(--text);font-size:0.85em;}
.history-filters input:focus,.history-filters select:focus{outline:none;border-color:var(--primary);}
.empty-state{text-align:center;padding:40px;color:var(--text3);}
@media(max-width:768px){.main{padding:12px;}.stats{grid-template-columns:1fr 1fr;}.tabs{flex-wrap:wrap;}.modal{width:96%;padding:20px;}th,td{padding:8px 6px;font-size:0.82em;}.actions{flex-direction:column;}}
</style>
</head>
<body>
<div class="top-bar">
    <h1>PTC Schedulatore</h1>
    <a href="/">Dashboard</a>
</div>
<div class="main">
    <div class="stats">
        <div class="stat-card"><div class="stat-label">Stato</div><div class="stat-value" id="sStatus">-</div></div>
        <div class="stat-card"><div class="stat-label">Task Totali</div><div class="stat-value" id="sTotal">-</div></div>
        <div class="stat-card"><div class="stat-label">Task Attivi</div><div class="stat-value" id="sActive">-</div></div>
        <div class="stat-card"><div class="stat-label">Esecuzioni Totali</div><div class="stat-value" id="sExecs">-</div></div>
        <div class="stat-card"><div class="stat-label">Cartella</div><div class="stat-value" id="sFolder" style="font-size:0.7em;word-break:break-all;">-</div></div>
    </div>
    <div class="tabs">
        <button class="tab active" onclick="switchTab('tasks',this)">Task</button>
        <button class="tab" onclick="switchTab('history',this)">Storico</button>
    </div>
    <div id="panel-tasks" class="panel active">
        <div class="card">
            <div class="card-header">
                <span class="card-title">Task Schedulati</span>
                <button class="btn btn-primary" onclick="openForm()">+ Nuovo Task</button>
            </div>
            <div style="overflow-x:auto;">
            <table>
                <thead><tr><th>Nome</th><th>Stato</th><th>Tipo</th><th>Programmazione</th><th>Comando</th><th>Ultima Esec.</th><th>#</th><th>Azioni</th></tr></thead>
                <tbody id="tBody"></tbody>
            </table>
            </div>
            <div id="tEmpty" class="empty-state" style="display:none;">Nessun task configurato. Crea il primo!</div>
        </div>
    </div>
    <div id="panel-history" class="panel">
        <div class="card">
            <div class="card-header"><span class="card-title">Storico Esecuzioni</span></div>
            <div class="history-filters">
                <input type="text" id="hFilter" placeholder="Filtra per nome task..." oninput="renderHistory()">
                <select id="hStatus" onchange="renderHistory()"><option value="">Tutti</option><option value="ok">Successo</option><option value="fail">Errore</option></select>
            </div>
            <div style="overflow-x:auto;">
            <table>
                <thead><tr><th>Data/Ora</th><th>Task</th><th>Comando</th><th>Esito</th><th>Codice</th></tr></thead>
                <tbody id="hBody"></tbody>
            </table>
            </div>
            <div id="hEmpty" class="empty-state" style="display:none;">Nessuna esecuzione registrata.</div>
        </div>
    </div>
</div>
<div id="formOverlay" class="overlay" style="display:none;" onclick="if(event.target===this)closeForm()">
    <div class="modal">
        <h2 id="fTitle">Nuovo Task</h2>
        <input type="hidden" id="fOrig" value="">
        <div class="field">
            <label>Nome Task</label>
            <input type="text" id="fName" placeholder="Es: Backup giornaliero">
        </div>
        <div class="field">
            <label>Modalita di Schedulazione</label>
            <div class="mode-switch">
                <button class="mode-btn active" onclick="setMode('schedule',this)">Giorno/Ora/Minuto</button>
                <button class="mode-btn" onclick="setMode('interval',this)">Intervallo (ogni N sec)</button>
            </div>
        </div>
        <div id="modeSchedule" class="mode-section active">
            <div class="field">
                <label>Giorni della Settimana</label>
                <div class="days-row" id="fDays"></div>
            </div>
            <div class="field">
                <label>Ore</label>
                <input type="text" id="fHours" placeholder="Es: 0,6,12,18">
                <div class="hint">Inserisci le ore separate da virgola (0-23)</div>
            </div>
            <div class="field">
                <label>Minuti</label>
                <input type="text" id="fMinutes" placeholder="Es: 0,15,30,45">
                <div class="hint">Inserisci i minuti separati da virgola (0-59)</div>
            </div>
        </div>
        <div id="modeInterval" class="mode-section">
            <div class="field">
                <label>Ripeti ogni (secondi)</label>
                <input type="number" id="fInterval" min="5" value="60" placeholder="60">
                <div class="hint">Minimo 5 secondi. Es: 60=ogni minuto, 3600=ogni ora</div>
            </div>
        </div>
        <div class="field">
            <label>Comando da Eseguire</label>
            <select id="fScriptSelect" onchange="if(this.value)document.getElementById('fCommand').value=this.value">
                <option value="">-- Seleziona da elenco --</option>
            </select>
            <input type="text" id="fCommand" placeholder="C:\Scripts\myscript.bat" style="margin-top:8px;">
            <div class="hint">Seleziona un file dall'elenco oppure scrivi il percorso manualmente</div>
        </div>
        <div class="modal-actions">
            <button class="btn btn-ghost" onclick="closeForm()">Annulla</button>
            <button class="btn btn-primary" onclick="saveTask()">Salva Task</button>
        </div>
    </div>
</div>
<script>
var T=[],H=[],scripts=[];
var curMode="schedule";
var DAYS=["Lu","Ma","Me","Gi","Ve","Sa","Do"];
function switchTab(id,el){document.querySelectorAll(".tab").forEach(function(t){t.classList.remove("active")});el.classList.add("active");document.querySelectorAll(".panel").forEach(function(p){p.classList.remove("active")});document.getElementById("panel-"+id).classList.add("active");}
function setMode(m,el){curMode=m;document.querySelectorAll(".mode-btn").forEach(function(b){b.classList.remove("active")});el.classList.add("active");document.getElementById("modeSchedule").className=m==="schedule"?"mode-section active":"mode-section";document.getElementById("modeInterval").className=m==="interval"?"mode-section active":"mode-section";}
function initDays(){var c=document.getElementById("fDays");c.innerHTML="";DAYS.forEach(function(d){var chip=document.createElement("div");chip.className="day-chip";chip.textContent=d;chip.setAttribute("data-day",d);chip.onclick=function(){this.classList.toggle("on")};c.appendChild(chip)});}
function loadData(){
    fetch("/api/scheduler").then(function(r){return r.json()}).then(function(data){
        T=data.tasks||[];H=data.history||[];
        var el=document.getElementById("sStatus");el.textContent=data.enabled?"Attivo":"Disattivo";el.className="stat-value "+(data.enabled?"on":"off");
        document.getElementById("sTotal").textContent=T.length;
        document.getElementById("sActive").textContent=T.filter(function(t){return t.enabled}).length;
        var totalExec=0;T.forEach(function(t){totalExec+=t.executionCount});document.getElementById("sExecs").textContent=totalExec;
        document.getElementById("sFolder").textContent=data.folder;
        renderTasks();renderHistory();
    }).catch(function(){});
    fetch("/api/scheduler/scripts").then(function(r){return r.json()}).then(function(data){
        scripts=data||[];
        var sel=document.getElementById("fScriptSelect");
        var curr=sel.value;
        sel.innerHTML="<option value=''>-- Seleziona da elenco ("+scripts.length+" file) --</option>";
        scripts.forEach(function(s){var o=document.createElement("option");o.value=s;o.textContent=s;sel.appendChild(o)});
        if(curr)sel.value=curr;
    }).catch(function(){});
}
function renderTasks(){
    var tb=document.getElementById("tBody");tb.innerHTML="";
    document.getElementById("tEmpty").style.display=T.length?"none":"block";
    T.forEach(function(t){
        var tr=document.createElement("tr");
        var isInt=t.intervalSeconds>0;
        var sched=isInt?("Ogni "+fmtInterval(t.intervalSeconds)):(t.days+" | ore:"+t.hours+" | min:"+t.minutes);
        var typeB=isInt?"<span class='badge badge-interval'>Intervallo</span>":"<span class='badge badge-schedule'>Programmato</span>";
        tr.innerHTML="<td><strong>"+esc(t.name)+"</strong></td>"
            +"<td><span class='badge "+(t.enabled?"badge-on":"badge-off")+"'>"+(t.enabled?"Attivo":"Off")+"</span></td>"
            +"<td>"+typeB+"</td>"
            +"<td>"+esc(sched)+"</td>"
            +"<td><span class='mono'>"+esc(t.command)+"</span></td>"
            +"<td>"+(t.lastExecution||"<span style='color:var(--text3)'>Mai</span>")+"</td>"
            +"<td>"+t.executionCount+"</td>"
            +"<td class='actions'>"
            +"<button class='btn btn-sm "+(t.enabled?"btn-warning":"btn-success")+"' onclick=\"togTask('"+esc(t.name)+"')\">"+(t.enabled?"Stop":"Avvia")+"</button>"
            +"<button class='btn btn-sm btn-ghost' onclick=\"editTask('"+esc(t.name)+"')\">Mod</button>"
            +"<button class='btn btn-sm btn-ghost' onclick=\"dupeTask('"+esc(t.name)+"')\">Dup</button>"
            +"<button class='btn btn-sm btn-danger' onclick=\"delTask('"+esc(t.name)+"')\">Elim</button>"
            +"</td>";
        tb.appendChild(tr);
    });
}
function renderHistory(){
    var filter=(document.getElementById("hFilter").value||"").toLowerCase();
    var status=document.getElementById("hStatus").value;
    var filtered=H.filter(function(e){
        if(filter&&e.taskName.toLowerCase().indexOf(filter)<0)return false;
        if(status==="ok"&&!e.success)return false;
        if(status==="fail"&&e.success)return false;
        return true;
    });
    var tb=document.getElementById("hBody");tb.innerHTML="";
    document.getElementById("hEmpty").style.display=filtered.length?"none":"block";
    filtered.forEach(function(e){
        var tr=document.createElement("tr");
        tr.innerHTML="<td>"+esc(e.timestamp)+"</td>"
            +"<td><strong>"+esc(e.taskName)+"</strong></td>"
            +"<td><span class='mono'>"+esc(e.command)+"</span></td>"
            +"<td><span class='badge "+(e.success?"badge-on":"badge-off")+"'>"+(e.success?"OK":"Errore")+"</span></td>"
            +"<td>"+e.exitCode+"</td>";
        tb.appendChild(tr);
    });
}
function openForm(task){
    document.getElementById("fTitle").textContent=task?"Modifica Task":"Nuovo Task";
    document.getElementById("fOrig").value=task?task.name:"";
    document.getElementById("fName").value=task?task.name:"";
    document.getElementById("fCommand").value=task?task.command:"";
    document.getElementById("fScriptSelect").value=task?task.command:"";
    var isInt=task&&task.intervalSeconds>0;
    if(isInt){
        curMode="interval";
        document.getElementById("fInterval").value=task.intervalSeconds;
    }else{
        curMode="schedule";
        document.getElementById("fHours").value=task?task.hours:"";
        document.getElementById("fMinutes").value=task?task.minutes:"";
    }
    var btns=document.querySelectorAll(".mode-btn");
    btns[0].className="mode-btn"+(curMode==="schedule"?" active":"");
    btns[1].className="mode-btn"+(curMode==="interval"?" active":"");
    document.getElementById("modeSchedule").className=curMode==="schedule"?"mode-section active":"mode-section";
    document.getElementById("modeInterval").className=curMode==="interval"?"mode-section active":"mode-section";
    initDays();
    if(task&&task.days){
        var ad=task.days.split(",");
        document.querySelectorAll("#fDays .day-chip").forEach(function(c){if(ad.indexOf(c.getAttribute("data-day"))>=0)c.classList.add("on")});
    }
    document.getElementById("formOverlay").style.display="flex";
}
function closeForm(){document.getElementById("formOverlay").style.display="none";}
function saveTask(){
    var name=document.getElementById("fName").value.trim();
    var cmd=document.getElementById("fCommand").value.trim();
    if(!name){alert("Inserisci un nome per il task");return;}
    if(!cmd){alert("Inserisci un comando da eseguire");return;}
    var data={originalName:document.getElementById("fOrig").value,name:name,command:cmd,enabled:"true",intervalSeconds:"0"};
    if(curMode==="interval"){
        var iv=parseInt(document.getElementById("fInterval").value)||0;
        if(iv<5){alert("Intervallo minimo: 5 secondi");return;}
        data.intervalSeconds=String(iv);
        data.days="";data.hours="";data.minutes="";
    }else{
        var days=[];document.querySelectorAll("#fDays .day-chip.on").forEach(function(c){days.push(c.getAttribute("data-day"))});
        if(!days.length){alert("Seleziona almeno un giorno");return;}
        data.days=days.join(",");
        data.hours=document.getElementById("fHours").value;
        data.minutes=document.getElementById("fMinutes").value;
        if(!data.hours){alert("Inserisci almeno un'ora");return;}
        if(!data.minutes){alert("Inserisci almeno un minuto");return;}
    }
    apiPost("/api/scheduler/save",data,function(){closeForm();loadData();});
}
function editTask(n){var t=T.find(function(x){return x.name===n});if(t)openForm(t);}
function dupeTask(n){var t=T.find(function(x){return x.name===n});if(t){var c={};for(var k in t)c[k]=t[k];c.name=t.name+" (copia)";openForm(c);document.getElementById("fOrig").value="";}}
function togTask(n){apiPost("/api/scheduler/toggle",{name:n},function(){loadData()});}
function delTask(n){if(!confirm("Eliminare il task '"+n+"'?"))return;apiPost("/api/scheduler/delete",{name:n},function(){loadData()});}
function apiPost(url,data,cb){
    var xhr=new XMLHttpRequest();xhr.open("POST",url,true);
    xhr.setRequestHeader("Content-Type","application/json");
    xhr.onload=function(){
        if(xhr.status===200){try{var r=JSON.parse(xhr.responseText);if(r.success){if(cb)cb();}else{alert("Errore: "+(r.error||"Operazione fallita"));}}catch(e){alert("Errore risposta server");}}
        else{alert("Errore HTTP: "+xhr.status);}
    };
    xhr.onerror=function(){alert("Errore di rete");};
    xhr.send(JSON.stringify(data));
}
function esc(s){if(!s)return"";var d=document.createElement("div");d.appendChild(document.createTextNode(s));return d.innerHTML;}
function fmtInterval(s){if(s>=86400)return Math.floor(s/86400)+"g "+Math.floor((s%86400)/3600)+"h";if(s>=3600)return Math.floor(s/3600)+"h "+Math.floor((s%3600)/60)+"m";if(s>=60)return Math.floor(s/60)+"m "+s%60+"s";return s+"s";}
initDays();loadData();setInterval(loadData,5000);
</script>
</body>
</html>)html";
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
            <p style="margin-top: 10px;"><a href="/scheduler" style="color: rgba(255,255,255,0.9); text-decoration: none; background: rgba(255,255,255,0.2); padding: 8px 20px; border-radius: 20px; font-weight: 600;">Schedulatore</a></p>
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
                        <span class="metric-label">Schedulatore</span>
                        <span class="metric-value" id="schedulerStatus">-</span>
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
                    document.getElementById('schedulerStatus').innerHTML = data.schedulerEnabled ?
                        '<span class="status active"></span>' + data.schedulerTasks + ' task' :
                        '<span class="status inactive"></span>Disattivo';
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
    else if (request.find("GET /scheduler") != std::string::npos) {
        std::string html = GetSchedulerPageHtml();
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
    else if (request.find("GET /api/scheduler/scripts") != std::string::npos) {
        std::string json = GetSchedulerScriptsJson();
        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(json.length()) + "\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "\r\n";
        response += json;
    }
    else if (request.find("GET /api/scheduler") != std::string::npos) {
        std::string json = GetSchedulerJson();
        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(json.length()) + "\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "\r\n";
        response += json;
    }
    else if (request.find("POST /api/scheduler/save") != std::string::npos) {
        std::string body = GetHttpRequestBody(request);
        std::string name = ExtractJsonValue(body, "name");
        std::string originalName = ExtractJsonValue(body, "originalName");
        std::string daysStr = ExtractJsonValue(body, "days");
        std::string hoursStr = ExtractJsonValue(body, "hours");
        std::string minutesStr = ExtractJsonValue(body, "minutes");
        std::string command = ExtractJsonValue(body, "command");
        std::string enabledStr = ExtractJsonValue(body, "enabled");
        std::string intervalStr = ExtractJsonValue(body, "intervalSeconds");

        std::string resultJson;

        if (name.empty() || command.empty()) {
            resultJson = "{\"success\": false, \"error\": \"Nome e comando sono obbligatori\"}";
        } else {
            // Se il nome originale e' diverso, elimina il vecchio file
            if (!originalName.empty() && originalName != name) {
                DeleteSchedulerTask(originalName);
            }

            SchedulerTask task;
            task.name = name;
            task.enabled = (enabledStr != "false");
            task.command = command;
            try { task.intervalSeconds = std::stoi(intervalStr); } catch (...) { task.intervalSeconds = 0; }

            // Parse days
            std::istringstream dss(daysStr);
            std::string day;
            while (std::getline(dss, day, ',')) {
                day.erase(0, day.find_first_not_of(" \t"));
                day.erase(day.find_last_not_of(" \t") + 1);
                int d = DayNameToNumber(day);
                if (d >= 0) task.days.insert(d);
            }

            // Parse hours
            std::istringstream hss(hoursStr);
            std::string hour;
            while (std::getline(hss, hour, ',')) {
                hour.erase(0, hour.find_first_not_of(" \t"));
                hour.erase(hour.find_last_not_of(" \t") + 1);
                try { int h = std::stoi(hour); if (h >= 0 && h <= 23) task.hours.insert(h); } catch (...) {}
            }

            // Parse minutes
            std::istringstream mss(minutesStr);
            std::string minute;
            while (std::getline(mss, minute, ',')) {
                minute.erase(0, minute.find_first_not_of(" \t"));
                minute.erase(minute.find_last_not_of(" \t") + 1);
                try { int m = std::stoi(minute); if (m >= 0 && m <= 59) task.minutes.insert(m); } catch (...) {}
            }

            if (SaveSchedulerTask(task)) {
                // Ricarica i task dal filesystem
                LoadSchedulerTasks();
                resultJson = "{\"success\": true}";
            } else {
                resultJson = "{\"success\": false, \"error\": \"Errore salvataggio su disco\"}";
            }
        }

        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(resultJson.length()) + "\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "\r\n";
        response += resultJson;
    }
    else if (request.find("POST /api/scheduler/delete") != std::string::npos) {
        std::string body = GetHttpRequestBody(request);
        std::string name = ExtractJsonValue(body, "name");

        std::string resultJson;
        if (!name.empty() && DeleteSchedulerTask(name)) {
            resultJson = "{\"success\": true}";
        } else {
            resultJson = "{\"success\": false, \"error\": \"Task non trovato\"}";
        }

        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(resultJson.length()) + "\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "\r\n";
        response += resultJson;
    }
    else if (request.find("POST /api/scheduler/toggle") != std::string::npos) {
        std::string body = GetHttpRequestBody(request);
        std::string name = ExtractJsonValue(body, "name");

        std::string resultJson = "{\"success\": false, \"error\": \"Task non trovato\"}";
        SchedulerTask taskCopy;
        bool found = false;

        if (!name.empty()) {
            {
                std::lock_guard<std::mutex> lock(schedulerMutex);
                for (auto& task : schedulerTasks) {
                    if (task.name == name) {
                        task.enabled = !task.enabled;
                        taskCopy = task;
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                SaveSchedulerTask(taskCopy);
                resultJson = "{\"success\": true, \"enabled\": " + std::string(taskCopy.enabled ? "true" : "false") + "}";
            }
        }

        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(resultJson.length()) + "\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "\r\n";
        response += resultJson;
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

    // Carica e avvia schedulatore
    LoadSchedulerTasks();
    if (schedulerEnabled) {
        schedulerThread = std::thread(SchedulerWorker);
        WriteToLog("Schedulatore avviato con " + std::to_string(schedulerTasks.size()) + " task");
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
    
    // 2. Ferma schedulatore (TIMEOUT: 2 secondi)
    if (schedulerThread.joinable()) {
        WriteToLog("Arresto thread schedulatore...");
        auto schedStartTime = GetTickCount();
        while (GetTickCount() - schedStartTime < 2000) {
            Sleep(100);
            if (!schedulerThread.joinable()) break;
        }
        try {
            if (schedulerThread.joinable()) {
                schedulerThread.join();
                WriteToLog("Thread schedulatore arrestato");
            }
        } catch (...) {
            WriteToLog("TIMEOUT schedulatore - detach forzato");
            schedulerThread.detach();
        }
    }

    // 3. Ferma monitor cartelle (TIMEOUT: 3 secondi)
    WriteToLog("Arresto monitor cartelle...");
    StopAllFolderMonitors();

    // 4. Ferma thread metriche (TIMEOUT: 1 secondo)
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
                std::cout << "Schedulatore: http://localhost:" << webServerPort << "/scheduler" << std::endl;
            }
            if (schedulerEnabled) {
                std::cout << "Schedulatore abilitato, cartella: " << schedulerFolder << std::endl;
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
