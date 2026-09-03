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
#include <cstring>

#include "miraFONT_embedded.h"

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
    std::vector<std::pair<std::string, time_t>> recentActivity;
    
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
std::atomic<int> webServerActiveConnections{0};
std::mutex reloadMutex;  // serializza il ricaricamento a caldo con le letture di stato dai thread HTTP

// Ricaricamento configurazione a caldo (richiesto dalla dashboard web)
std::atomic<bool> configReloadRequested{false};
std::string lastConfigReloadTime;   // protetto da configMutex
std::string lastConfigReloadError;  // protetto da configMutex

// Statistiche storiche per i grafici della dashboard
struct StatsSample {
    time_t timestamp;
    size_t filesProcessed;
    size_t commandsExecuted;
    size_t errors;
    size_t memoryMB;
};
#define MAX_STATS_SAMPLES 1440
#define STATS_SAMPLE_INTERVAL_SEC 60
std::vector<StatsSample> statsSamples;
std::mutex statsMutex;
std::atomic<size_t> hourlyFiles[24];
std::atomic<size_t> hourlyCommands[24];
int statsCurrentDay = -1;

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

// Web: pagine, configurazione, statistiche
std::string GetSettingsPageHtml();
std::string GetConfigJson();
std::string GetStatsJson();
void RecordStatsSample();
void CheckDailyStatsRollover();
void RecordHourlyFile();
void RecordHourlyCommand();
void StartWebServer();
void StopWebServer();
void ApplyConfigurationReload();

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
        systemMetrics.recentActivity.push_back({message, std::time(NULL)});
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
    RecordHourlyFile();
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
                try { webServerPort = std::stoi(value); } catch (...) { webServerPort = DEFAULT_WEB_PORT; }
                if (webServerPort < 1 || webServerPort > 65535) webServerPort = DEFAULT_WEB_PORT;
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
            
            // Il primo campo e' una cartella se assomiglia a un percorso (es. C:\..., \\server\...);
            // altrimenti e' una regex (formato legacy). La regex puo' contenere '|' solo se la
            // cartella e' indicata esplicitamente: i campi intermedi vengono riuniti.
            auto looksLikePath = [](const std::string& s) {
                return (s.length() >= 2 && s[1] == ':') || (s.length() >= 2 && s[0] == '\\' && s[1] == '\\') || (!s.empty() && s[0] == '/');
            };
            if (parts.size() >= 3 && looksLikePath(parts[0])) {
                folderPath = parts[0];
                pattern = parts[1];
                for (size_t pi = 2; pi + 1 < parts.size(); ++pi) pattern += "|" + parts[pi];
                command = parts.back();
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
        WriteToLog("AVVISO: Nessun pattern valido trovato: nessuna cartella verra' monitorata (configurabile dalla dashboard web)");
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
        RecordHourlyCommand();
        
        // Aggiorna contatore esecuzioni
        std::lock_guard<std::mutex> lock(patternStatsMutex);
        patternExecutionCounts[patternName]++;
        
    } else if (waitResult == WAIT_TIMEOUT) {
        WriteToLog("TIMEOUT: Processo terminato forzatamente");
        TerminateProcess(pi.hProcess, 1);
        MarkFileAsProcessed(parameter);
        success = true;
        systemMetrics.commandsExecuted++;
        RecordHourlyCommand();
        
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
    std::lock_guard<std::mutex> reloadLock(reloadMutex);
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

    CheckDailyStatsRollover();
    UpdateSystemMetrics();
    RecordStatsSample();
    time_t lastSample = std::time(NULL);

    while (!globalShutdown) {
        UpdateSystemMetrics();
        CheckDailyStatsRollover();
        if (std::time(NULL) - lastSample >= STATS_SAMPLE_INTERVAL_SEC) {
            RecordStatsSample();
            lastSample = std::time(NULL);
        }
        // Sleep frazionato per rispondere rapidamente a globalShutdown
        for (int i = 0; i < 50 && !globalShutdown; ++i) {
            Sleep(100);
        }
    }

    WriteToLog("Thread aggiornamento metriche terminato");
}

std::string GetSystemMetricsJson() {
    std::lock_guard<std::mutex> reloadLock(reloadMutex);
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
    json << "  \"webServerPort\": " << webServerPort << ",\n";
    json << "  \"reloadPending\": " << (configReloadRequested ? "true" : "false") << ",\n";
    json << "  \"configFile\": \"" << EscapeJsonString(configFile) << "\",\n";
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
        long long timestamp = static_cast<long long>(activity.second);
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
                char e = json[i + 1];
                switch (e) {
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'u':
                        if (i + 5 < json.length()) {
                            unsigned int cp = 0;
                            try { cp = static_cast<unsigned int>(std::stoul(json.substr(i + 2, 4), NULL, 16)); } catch (...) { cp = '?'; }
                            if (cp < 0x80) result += static_cast<char>(cp);
                            else if (cp < 0x800) { result += static_cast<char>(0xC0 | (cp >> 6)); result += static_cast<char>(0x80 | (cp & 0x3F)); }
                            else { result += static_cast<char>(0xE0 | (cp >> 12)); result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); result += static_cast<char>(0x80 | (cp & 0x3F)); }
                            i += 4;
                        }
                        break;
                    default: result += e; break;
                }
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
            for (int i = 0; i < 150 && !globalShutdown; ++i) Sleep(100);
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

        // Sleep frazionato per rispondere rapidamente a globalShutdown
        for (int i = 0; i < 150 && !globalShutdown; ++i) Sleep(100);
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

// ====== INTERFACCIA WEB: STILE E LAYOUT COMUNI ======
// Autore: Umberto Meglio - Supporto alla creazione: Claude di Anthropic

std::string GetCommonCss() {
    return R"css(
@font-face{font-family:'miraFONT Spline';font-style:normal;font-weight:400;font-display:swap;src:url('/fonts/miraFONT-Regular-Spline.woff') format('woff');}
@font-face{font-family:'miraFONT Spline';font-style:normal;font-weight:700;font-display:swap;src:url('/fonts/miraFONT-Bold-Spline.woff') format('woff');}
@font-face{font-family:'miraFONT Linear';font-style:normal;font-weight:400;font-display:swap;src:url('/fonts/miraFONT-Regular-Linear.woff') format('woff');}
@font-face{font-family:'miraFONT';font-style:normal;font-weight:400;font-display:swap;src:url('/fonts/miraFONT-Regular-Dots.woff') format('woff');}
:root{--bg:#03060f;--bg-2:#081026;--panel:#0a1430;--panel-2:#060d20;--surface:#0a1430;--surface2:#060d20;--line:rgba(47,208,255,0.16);--line-strong:rgba(47,208,255,0.4);--border:rgba(47,208,255,0.16);--cyan:#2fd0ff;--cyan-soft:#a8ecff;--blue:#1e7bff;--primary:#2fd0ff;--primary-dark:#1e7bff;--primary-bg:rgba(47,208,255,0.08);--green:#7dffb0;--green-bg:rgba(125,255,176,0.1);--red:#ff5f57;--red-bg:rgba(255,95,87,0.12);--warning:#ffcf6b;--warning-bg:rgba(255,207,107,0.12);--purple:#c792ea;--purple-bg:rgba(199,146,234,0.12);--text:#d6ecff;--text2:#8fb6e0;--text3:#6a82ab;--radius:11px;--shadow:0 0 30px rgba(47,208,255,0.06),inset 0 0 20px rgba(0,0,0,0.35);--font-display:'miraFONT Spline','Courier New',monospace;--font-body:'miraFONT Linear',ui-monospace,'Cascadia Mono','Courier New',monospace;--font-dots:'miraFONT',ui-monospace,'Courier New',monospace;}
*{margin:0;padding:0;box-sizing:border-box;}
html{scroll-behavior:smooth;}
body{font-family:var(--font-body);font-size:15px;line-height:1.6;background-color:var(--bg);background-image:radial-gradient(ellipse 80% 50% at 50% -10%,rgba(30,123,255,0.14),transparent),linear-gradient(rgba(47,208,255,0.025) 1px,transparent 1px),linear-gradient(90deg,rgba(47,208,255,0.025) 1px,transparent 1px);background-size:100% 100%,40px 40px,40px 40px;color:var(--text);min-height:100vh;display:flex;flex-direction:column;-webkit-font-smoothing:antialiased;}
a{color:var(--cyan);}
h1,h2,h3,.stat-label,.card-title,.brand-name,th,.tab,.btn,.page-head h2{font-family:var(--font-display);}
.top-bar{background:rgba(3,6,15,0.85);backdrop-filter:blur(10px);padding:0 24px;height:62px;display:flex;align-items:center;justify-content:space-between;position:sticky;top:0;z-index:50;border-bottom:1px solid var(--line);gap:16px;}
.brand{display:flex;align-items:center;gap:12px;color:var(--cyan);text-decoration:none;min-width:0;}
.brand svg{width:34px;height:34px;flex-shrink:0;filter:drop-shadow(0 0 8px rgba(47,208,255,0.5));}
.brand-name{font-size:0.95em;font-weight:700;letter-spacing:0.2em;text-transform:uppercase;white-space:nowrap;text-shadow:0 0 14px rgba(47,208,255,0.45);}
.brand-sub{font-size:0.72em;color:var(--text3);display:block;margin-top:-1px;white-space:nowrap;letter-spacing:0.08em;}
.nav{display:flex;gap:4px;align-items:center;}
.nav a{color:var(--text2);text-decoration:none;font-size:0.82em;font-weight:700;letter-spacing:0.08em;text-transform:uppercase;padding:8px 16px;border-radius:999px;border:1px solid transparent;transition:all 0.25s;display:inline-flex;align-items:center;gap:7px;white-space:nowrap;}
.nav a svg{width:15px;height:15px;}
.nav a:hover{color:var(--cyan);border-color:var(--line);background:var(--primary-bg);}
.nav a.active{background:var(--cyan);color:#04121a;box-shadow:0 0 20px rgba(47,208,255,0.35);}
.live{display:flex;align-items:center;gap:8px;color:var(--text2);font-size:0.78em;letter-spacing:0.06em;white-space:nowrap;}
.live .dot{width:9px;height:9px;}
.live .dot-on{animation:pulse 1.6s infinite;}
@keyframes pulse{0%{box-shadow:0 0 0 0 rgba(125,255,176,0.6);}70%{box-shadow:0 0 0 7px rgba(125,255,176,0);}100%{box-shadow:0 0 0 0 rgba(125,255,176,0);}}
.top-stripe{height:2px;background:linear-gradient(90deg,transparent,var(--cyan) 30%,var(--blue) 70%,transparent);opacity:0.8;}
.main{max-width:1440px;width:100%;margin:0 auto;padding:26px 24px;flex:1;}
.prompt{font-family:var(--font-body);color:var(--text3);font-size:0.8em;margin-bottom:6px;letter-spacing:0.04em;}
.prompt b{color:var(--green);font-weight:400;}
.prompt i{color:var(--cyan);font-style:normal;}
.page-head{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;margin-bottom:22px;flex-wrap:wrap;}
.page-head h2{font-size:1.6em;font-weight:700;color:var(--cyan-soft);letter-spacing:0.04em;text-shadow:0 0 18px rgba(47,208,255,0.25);}
.page-head p{color:var(--text2);font-size:0.88em;margin-top:4px;max-width:820px;}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:14px;margin-bottom:22px;}
.stat-card{background:linear-gradient(180deg,var(--panel),var(--panel-2));border:1px solid var(--line);border-radius:var(--radius);padding:16px 18px;box-shadow:var(--shadow);display:flex;gap:14px;align-items:center;transition:transform 0.3s,border-color 0.3s,box-shadow 0.3s;min-width:0;position:relative;overflow:hidden;}
.stat-card:before{content:"";position:absolute;inset:0 0 auto 0;height:1px;background:linear-gradient(90deg,transparent,rgba(47,208,255,0.5),transparent);opacity:0;transition:opacity 0.3s;}
.stat-card:hover{border-color:var(--line-strong);transform:translateY(-3px);box-shadow:0 10px 30px rgba(0,0,0,0.4),0 0 24px rgba(47,208,255,0.12);}
.stat-card:hover:before{opacity:1;}
.stat-icon{width:42px;height:42px;border-radius:11px;display:flex;align-items:center;justify-content:center;flex-shrink:0;background:var(--primary-bg);color:var(--cyan);border:1px solid var(--line);}
.stat-icon svg{width:21px;height:21px;}
.stat-icon.green{background:var(--green-bg);color:var(--green);border-color:rgba(125,255,176,0.25);}
.stat-icon.red{background:var(--red-bg);color:var(--red);border-color:rgba(255,95,87,0.3);}
.stat-icon.warn{background:var(--warning-bg);color:var(--warning);border-color:rgba(255,207,107,0.3);}
.stat-icon.purple{background:var(--purple-bg);color:var(--purple);border-color:rgba(199,146,234,0.3);}
.stat-body{min-width:0;flex:1;}
.stat-label{font-size:0.68em;color:var(--text3);text-transform:uppercase;letter-spacing:0.18em;margin-bottom:4px;}
.stat-value{font-family:var(--font-dots);font-size:1.5em;color:var(--cyan-soft);line-height:1.15;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;text-shadow:0 0 12px rgba(47,208,255,0.35);}
.stat-value.small{font-family:var(--font-body);font-size:0.85em;white-space:normal;word-break:break-all;text-shadow:none;color:var(--text);}
.stat-value.on{color:var(--green);text-shadow:0 0 12px rgba(125,255,176,0.4);}
.stat-value.off,.stat-value.alert{color:var(--red);text-shadow:0 0 12px rgba(255,95,87,0.4);}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-bottom:16px;}
.grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:16px;margin-bottom:16px;}
.card{background:linear-gradient(180deg,var(--panel),var(--panel-2));border:1px solid var(--line);border-radius:var(--radius);padding:20px 22px;margin-bottom:16px;box-shadow:var(--shadow);}
.card-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px;gap:12px;flex-wrap:wrap;}
.card-title{font-size:0.88em;font-weight:700;color:var(--cyan);display:flex;align-items:center;gap:9px;letter-spacing:0.14em;text-transform:uppercase;text-shadow:0 0 12px rgba(47,208,255,0.3);}
.card-title svg{width:17px;height:17px;}
.card-title .count{font-family:var(--font-dots);font-size:0.85em;color:var(--text2);background:rgba(47,208,255,0.08);padding:2px 10px;border-radius:999px;border:1px solid var(--line);letter-spacing:0;text-shadow:none;}
.card-desc{font-size:0.84em;color:var(--text2);margin:-6px 0 14px;}
.table-wrap{overflow-x:auto;border:1px solid var(--line);border-radius:9px;background:#010409;}
table{width:100%;border-collapse:collapse;}
th{text-align:left;padding:10px 14px;font-size:0.68em;color:var(--text3);text-transform:uppercase;letter-spacing:0.16em;border-bottom:1px solid var(--line);font-weight:400;background:rgba(47,208,255,0.04);white-space:nowrap;}
td{padding:10px 14px;border-bottom:1px solid rgba(47,208,255,0.08);font-size:0.88em;vertical-align:middle;}
tbody tr:last-child td{border-bottom:none;}
tbody tr:hover td{background:rgba(47,208,255,0.05);}
td.num{text-align:right;font-family:var(--font-dots);color:var(--cyan-soft);}
.kv{width:100%;}
.kv td{padding:9px 4px;}
.kv td:first-child{color:var(--text2);}
.kv td:last-child{text-align:right;color:var(--cyan-soft);}
.badge{display:inline-flex;align-items:center;gap:6px;padding:3px 11px;border-radius:999px;font-size:0.74em;letter-spacing:0.06em;white-space:nowrap;border:1px solid var(--line);background:rgba(47,208,255,0.07);color:var(--text2);}
.badge-on{background:var(--green-bg);color:var(--green);border-color:rgba(125,255,176,0.3);}
.badge-off{background:var(--red-bg);color:var(--red);border-color:rgba(255,95,87,0.3);}
.badge-warn{background:var(--warning-bg);color:var(--warning);border-color:rgba(255,207,107,0.3);}
.badge-info{background:var(--primary-bg);color:var(--cyan);}
.badge-interval{background:var(--purple-bg);color:var(--purple);border-color:rgba(199,146,234,0.3);}
.badge-schedule{background:var(--warning-bg);color:var(--warning);border-color:rgba(255,207,107,0.3);}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;flex-shrink:0;}
.dot-on{background:var(--green);box-shadow:0 0 8px rgba(125,255,176,0.8);}
.dot-off{background:var(--red);box-shadow:0 0 6px rgba(255,95,87,0.6);}
.mono{font-family:var(--font-body);font-size:0.86em;color:var(--cyan-soft);background:rgba(47,208,255,0.07);padding:2px 8px;border-radius:5px;word-break:break-all;border:1px solid rgba(47,208,255,0.1);}
.muted{color:var(--text3);}
.btn{padding:9px 16px;border:1px solid transparent;border-radius:9px;cursor:pointer;font-size:0.78em;font-weight:700;letter-spacing:0.08em;text-transform:uppercase;transition:all 0.2s;display:inline-flex;align-items:center;gap:7px;line-height:1.2;text-decoration:none;}
.btn svg{width:14px;height:14px;}
.btn:hover{transform:translateY(-1px);}
.btn:active{transform:translateY(0);box-shadow:none;}
.btn:disabled{opacity:0.5;cursor:not-allowed;transform:none;box-shadow:none;}
.btn-primary{background:var(--cyan);color:#04121a;box-shadow:0 0 20px rgba(47,208,255,0.3);}
.btn-primary:hover{box-shadow:0 0 28px rgba(47,208,255,0.5);}
.btn-success{background:var(--green);color:#04121a;box-shadow:0 0 18px rgba(125,255,176,0.3);}
.btn-warning{background:var(--warning);color:#2a1a00;box-shadow:0 0 18px rgba(255,207,107,0.25);}
.btn-danger{background:transparent;color:var(--red);border-color:rgba(255,95,87,0.4);}
.btn-danger:hover{background:var(--red-bg);box-shadow:0 0 16px rgba(255,95,87,0.25);}
.btn-ghost{background:transparent;color:var(--text2);border-color:var(--line);}
.btn-ghost:hover{color:var(--cyan);border-color:var(--line-strong);background:var(--primary-bg);}
.btn-sm{padding:5px 11px;font-size:0.7em;}
.btn-icon{padding:6px;width:30px;height:30px;justify-content:center;}
.actions{display:flex;gap:5px;flex-wrap:wrap;align-items:center;}
.tabs{display:inline-flex;gap:4px;margin-bottom:18px;background:#010409;border-radius:10px;padding:4px;border:1px solid var(--line);}
.tab{padding:9px 22px;border-radius:7px;cursor:pointer;font-weight:700;font-size:0.78em;letter-spacing:0.1em;text-transform:uppercase;color:var(--text2);transition:all 0.2s;border:none;background:transparent;}
.tab:hover{color:var(--cyan);background:var(--primary-bg);}
.tab.active{background:var(--cyan);color:#04121a;box-shadow:0 0 16px rgba(47,208,255,0.3);}
.panel{display:none;}
.panel.active{display:block;animation:fade 0.25s ease;}
@keyframes fade{from{opacity:0;transform:translateY(4px);}to{opacity:1;transform:none;}}
.overlay{position:fixed;inset:0;background:rgba(1,4,9,0.7);display:flex;align-items:center;justify-content:center;z-index:100;backdrop-filter:blur(4px);padding:16px;}
.modal{background:linear-gradient(180deg,var(--panel),var(--panel-2));border:1px solid var(--line-strong);border-radius:14px;padding:26px 28px;width:100%;max-width:640px;max-height:92vh;overflow-y:auto;box-shadow:0 20px 60px rgba(0,0,0,0.6),0 0 40px rgba(47,208,255,0.1);animation:fade 0.2s ease;}
.modal h2{font-size:1em;letter-spacing:0.14em;text-transform:uppercase;margin-bottom:18px;color:var(--cyan);padding-bottom:12px;border-bottom:1px solid var(--line);}
.field{margin-bottom:16px;min-width:0;}
.field label{display:block;font-size:0.72em;letter-spacing:0.12em;text-transform:uppercase;color:var(--text2);margin-bottom:6px;}
.field input[type=text],.field input[type=number],.field select,.field textarea,.input{width:100%;padding:9px 12px;background:#010409;border:1px solid var(--line);border-radius:8px;color:var(--text);font-size:0.9em;font-family:var(--font-body);transition:border-color 0.2s,box-shadow 0.2s;}
.field input:focus,.field select:focus,.field textarea:focus,.input:focus{outline:none;border-color:var(--cyan);box-shadow:0 0 0 3px rgba(47,208,255,0.15);}
.field select{cursor:pointer;}
.field select option{background:#0a1430;color:var(--text);}
.field .hint{font-size:0.74em;color:var(--text3);margin-top:5px;}
.form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:4px 22px;}
.switch{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 14px;border:1px solid var(--line);border-radius:9px;background:#010409;margin-bottom:12px;}
.switch .sw-text{min-width:0;}
.switch .sw-title{font-family:var(--font-display);font-size:0.86em;color:var(--cyan-soft);letter-spacing:0.04em;}
.switch .sw-desc{font-size:0.74em;color:var(--text3);margin-top:2px;}
.toggle{position:relative;width:44px;height:24px;flex-shrink:0;}
.toggle input{opacity:0;width:0;height:0;}
.toggle span{position:absolute;inset:0;background:#1a2a4a;border-radius:24px;cursor:pointer;transition:0.2s;border:1px solid var(--line);}
.toggle span:before{content:"";position:absolute;width:16px;height:16px;left:3px;top:3px;background:var(--text2);border-radius:50%;transition:0.2s;}
.toggle input:checked+span{background:rgba(125,255,176,0.25);border-color:var(--green);}
.toggle input:checked+span:before{transform:translateX(20px);background:var(--green);box-shadow:0 0 8px rgba(125,255,176,0.8);}
.days-row{display:flex;gap:6px;flex-wrap:wrap;}
.day-chip{padding:8px 14px;border-radius:8px;cursor:pointer;font-size:0.86em;background:#010409;border:1px solid var(--line);color:var(--text2);transition:all 0.15s;user-select:none;}
.day-chip.on{background:var(--primary-bg);border-color:var(--cyan);color:var(--cyan);box-shadow:0 0 10px rgba(47,208,255,0.2);}
.mode-switch{display:flex;gap:4px;background:#010409;border-radius:8px;padding:4px;margin-bottom:12px;border:1px solid var(--line);}
.mode-btn{flex:1;padding:9px;border:none;border-radius:6px;cursor:pointer;font-family:var(--font-display);font-size:0.78em;letter-spacing:0.06em;color:var(--text2);background:transparent;transition:all 0.2s;}
.mode-btn.active{background:var(--cyan);color:#04121a;}
.mode-section{display:none;}
.mode-section.active{display:block;}
.modal-actions{display:flex;gap:10px;justify-content:flex-end;margin-top:22px;padding-top:16px;border-top:1px solid var(--line);}
.filters{display:flex;gap:10px;margin-bottom:14px;flex-wrap:wrap;}
.filters input,.filters select{padding:8px 12px;background:#010409;border:1px solid var(--line);border-radius:8px;color:var(--text);font-size:0.85em;font-family:var(--font-body);}
.filters select option{background:#0a1430;}
.filters input:focus,.filters select:focus{outline:none;border-color:var(--cyan);}
.empty-state{text-align:center;padding:36px 16px;color:var(--text3);font-size:0.88em;}
.activity-list{max-height:340px;overflow-y:auto;margin:0 -6px;padding:0 6px;}
.activity-item{padding:9px 0;border-bottom:1px solid rgba(47,208,255,0.08);display:flex;gap:12px;align-items:flex-start;font-size:0.84em;}
.activity-item:last-child{border-bottom:none;}
.activity-item .adot{width:7px;height:7px;border-radius:50%;background:var(--cyan);margin-top:7px;flex-shrink:0;box-shadow:0 0 6px rgba(47,208,255,0.6);}
.activity-item.err .adot{background:var(--red);box-shadow:0 0 6px rgba(255,95,87,0.6);}
.activity-item.ok .adot{background:var(--green);box-shadow:0 0 6px rgba(125,255,176,0.6);}
.activity-msg{color:var(--text2);flex:1;word-break:break-word;}
.activity-time{color:var(--text3);font-size:0.8em;white-space:nowrap;font-family:var(--font-dots);}
.loading{text-align:center;padding:60px;color:var(--text2);font-size:1em;letter-spacing:0.1em;}
.alert{padding:13px 16px;border-radius:10px;margin-bottom:16px;font-size:0.88em;display:flex;gap:10px;align-items:flex-start;line-height:1.5;border:1px solid;}
.alert svg{width:18px;height:18px;flex-shrink:0;margin-top:1px;}
.alert-error{background:var(--red-bg);border-color:rgba(255,95,87,0.4);color:#ffb3af;}
.alert-warn{background:var(--warning-bg);border-color:rgba(255,207,107,0.4);color:#ffe4ad;}
.alert-info{background:var(--primary-bg);border-color:var(--line-strong);color:var(--cyan-soft);}
.alert-ok{background:var(--green-bg);border-color:rgba(125,255,176,0.4);color:var(--green);}
.toasts{position:fixed;right:20px;bottom:20px;display:flex;flex-direction:column;gap:10px;z-index:200;max-width:380px;}
.toast{background:#010409;color:var(--text);padding:12px 16px;border-radius:10px;box-shadow:0 8px 24px rgba(0,0,0,0.5),0 0 20px rgba(47,208,255,0.1);font-size:0.86em;display:flex;gap:10px;align-items:center;animation:fade 0.25s ease;border:1px solid var(--line);border-left:3px solid var(--cyan);}
.toast.ok{border-left-color:var(--green);}
.toast.err{border-left-color:var(--red);}
.toast.warn{border-left-color:var(--warning);}
.save-bar{position:sticky;bottom:0;background:rgba(3,6,15,0.92);backdrop-filter:blur(10px);border:1px solid var(--line-strong);border-radius:var(--radius);padding:14px 20px;display:flex;justify-content:space-between;align-items:center;gap:12px;box-shadow:0 -6px 30px rgba(0,0,0,0.5);margin-top:8px;flex-wrap:wrap;}
.save-bar .status{font-size:0.84em;color:var(--text2);display:flex;align-items:center;gap:8px;}
.save-bar .status svg{width:16px;height:16px;color:var(--green);}
.save-bar .status.dirty{color:var(--warning);}
.pattern-table td{padding:6px 8px;}
.pattern-table input{width:100%;padding:7px 10px;border:1px solid var(--line);border-radius:7px;font-size:0.86em;font-family:var(--font-body);background:#010409;color:var(--text);}
.pattern-table input.mono-in{color:var(--cyan-soft);}
.pattern-table input:focus{outline:none;border-color:var(--cyan);box-shadow:0 0 0 3px rgba(47,208,255,0.15);}
.pattern-table input.bad{border-color:var(--red);background:var(--red-bg);}
.pattern-table input.hit{border-color:var(--green);background:var(--green-bg);}
.chart-wrap{position:relative;width:100%;}
.chart-wrap canvas{width:100%;height:190px;display:block;}
.chart-legend{display:flex;gap:16px;flex-wrap:wrap;font-size:0.74em;color:var(--text2);margin-top:8px;letter-spacing:0.04em;}
.chart-legend span{display:inline-flex;align-items:center;gap:6px;}
.chart-legend i{width:12px;height:3px;border-radius:2px;display:inline-block;}
.bars{display:flex;flex-direction:column;gap:9px;}
.bar-row{display:grid;grid-template-columns:150px 1fr 60px;align-items:center;gap:10px;font-size:0.84em;}
.bar-row .bar-label{color:var(--text2);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.bar-row .bar-track{height:10px;background:#010409;border:1px solid var(--line);border-radius:999px;overflow:hidden;}
.bar-row .bar-fill{height:100%;background:linear-gradient(90deg,var(--blue),var(--cyan));border-radius:999px;box-shadow:0 0 10px rgba(47,208,255,0.5);transition:width 0.6s ease;}
.bar-row .bar-fill.green{background:linear-gradient(90deg,#1f9d5a,var(--green));box-shadow:0 0 10px rgba(125,255,176,0.5);}
.bar-row .bar-fill.red{background:linear-gradient(90deg,#a33,var(--red));box-shadow:0 0 10px rgba(255,95,87,0.5);}
.bar-row .bar-val{font-family:var(--font-dots);color:var(--cyan-soft);text-align:right;}
.donut{display:flex;align-items:center;gap:18px;}
.donut svg{width:110px;height:110px;flex-shrink:0;}
.donut .dlist{font-size:0.84em;color:var(--text2);display:flex;flex-direction:column;gap:6px;}
.donut .dlist b{font-family:var(--font-dots);color:var(--cyan-soft);font-weight:400;margin-left:6px;}
.footer{text-align:center;padding:18px;color:var(--text3);font-size:0.74em;letter-spacing:0.06em;border-top:1px solid var(--line);background:rgba(1,4,9,0.6);}
.footer strong{color:var(--cyan);font-family:var(--font-display);letter-spacing:0.12em;}
.footer a{color:var(--text2);}
::-webkit-scrollbar{width:10px;height:10px;}
::-webkit-scrollbar-track{background:#010409;}
::-webkit-scrollbar-thumb{background:#1a2a4a;border-radius:5px;border:2px solid #010409;}
::-webkit-scrollbar-thumb:hover{background:var(--blue);}
@media(max-width:1100px){.grid3{grid-template-columns:1fr;}}
@media(max-width:900px){.grid2{grid-template-columns:1fr;}.brand-sub{display:none;}}
@media(max-width:700px){.top-bar{padding:0 12px;height:auto;min-height:56px;flex-wrap:wrap;padding-bottom:8px;}.nav{order:3;width:100%;justify-content:space-around;}.nav a{padding:7px 10px;font-size:0.74em;}.nav a span{display:none;}.live span{display:none;}.main{padding:12px;}.stats{grid-template-columns:1fr 1fr;gap:10px;}.stat-card{padding:12px;}th,td{padding:8px 8px;font-size:0.8em;}.modal{padding:18px;}.card{padding:14px;}.form-grid{grid-template-columns:1fr;}.toasts{left:12px;right:12px;max-width:none;}.bar-row{grid-template-columns:100px 1fr 50px;}}
)css";
}

// Icone SVG inline (stroke, 24x24) usate nelle pagine
#define ICO_HOME "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M3 11l9-8 9 8v9a2 2 0 0 1-2 2h-4v-6H9v6H5a2 2 0 0 1-2-2z'/></svg>"
#define ICO_CLOCK "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='12' r='9'/><path d='M12 7v5l3 2'/></svg>"
#define ICO_GEAR "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='12' r='3'/><path d='M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z'/></svg>"
#define ICO_FILE "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z'/><path d='M14 2v6h6'/></svg>"
#define ICO_CAL "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><rect x='3' y='4' width='18' height='18' rx='2'/><path d='M16 2v4M8 2v4M3 10h18'/></svg>"
#define ICO_PLAY "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M5 3l14 9-14 9z'/></svg>"
#define ICO_ALERT "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M10.3 3.9L1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z'/><path d='M12 9v4M12 17h.01'/></svg>"
#define ICO_CHIP "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><rect x='4' y='4' width='16' height='16' rx='2'/><rect x='9' y='9' width='6' height='6'/><path d='M9 1v3M15 1v3M9 20v3M15 20v3M20 9h3M20 14h3M1 9h3M1 14h3'/></svg>"
#define ICO_LAYERS "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M12 2l10 5-10 5L2 7z'/><path d='M2 17l10 5 10-5M2 12l10 5 10-5'/></svg>"
#define ICO_ACTIVITY "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M22 12h-4l-3 9L9 3l-3 9H2'/></svg>"
#define ICO_FOLDER "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/></svg>"
#define ICO_REGEX "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M17 3v10M12.7 5.5l8.6 5M12.7 10.5l8.6-5'/><rect x='3' y='15' width='6' height='6' rx='1'/></svg>"
#define ICO_PLUS "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M12 5v14M5 12h14'/></svg>"
#define ICO_TRASH "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M3 6h18M8 6V4a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1v2M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6'/></svg>"
#define ICO_SAVE "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z'/><path d='M17 21v-8H7v8M7 3v5h8'/></svg>"
#define ICO_REFRESH "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M23 4v6h-6M1 20v-6h6'/><path d='M3.5 9a9 9 0 0 1 14.9-3.4L23 10M1 14l4.6 4.4A9 9 0 0 0 20.5 15'/></svg>"
#define ICO_EDIT "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7'/><path d='M18.5 2.5a2.1 2.1 0 0 1 3 3L12 15l-4 1 1-4z'/></svg>"
#define ICO_COPY "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><rect x='9' y='9' width='13' height='13' rx='2'/><path d='M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1'/></svg>"
#define ICO_INFO "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='12' r='9'/><path d='M12 16v-4M12 8h.01'/></svg>"
#define ICO_CHECK "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M20 6L9 17l-5-5'/></svg>"
#define ICO_MEM "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><rect x='2' y='6' width='20' height='12' rx='2'/><path d='M6 10v4M10 10v4M14 10v4M18 10v4'/></svg>"
#define ICO_TERMINAL "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M4 17l6-6-6-6M12 19h8'/></svg>"
#define ICO_GLOBE "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='12' r='9'/><path d='M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18'/></svg>"

// Favicon inline (SVG codificato come data URI): cerchio azzurro con lente/pattern
#define PTC_FAVICON "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Ccircle cx='16' cy='16' r='15' fill='%230087cd'/%3E%3Cpath d='M9 10h8a4 4 0 0 1 0 8h-4v5' fill='none' stroke='white' stroke-width='3' stroke-linecap='round'/%3E%3Ccircle cx='23' cy='9' r='3' fill='%2300c96b'/%3E%3C/svg%3E"

std::string GetPageHeader(const std::string& title, const std::string& active) {
    std::string h;
    h += "<!DOCTYPE html>\n<html lang=\"it\">\n<head>\n<meta charset=\"UTF-8\">\n";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    h += "<title>PTC - " + title + "</title>\n";
    h += "<link rel=\"icon\" href=\"" PTC_FAVICON "\">\n";
    h += "<style>" + GetCommonCss() + "</style>\n</head>\n<body>\n";
    h += "<div class=\"top-bar\">\n";
    h += "  <a class=\"brand\" href=\"/\">";
    h += "<svg viewBox='0 0 32 32'><circle cx='16' cy='16' r='15' fill='white' opacity='0.18'/><circle cx='16' cy='16' r='13' fill='white'/><path d='M10 10h7a4 4 0 0 1 0 8h-4v5' fill='none' stroke='#0087cd' stroke-width='3' stroke-linecap='round'/><circle cx='22.5' cy='9.5' r='3' fill='#00c96b'/></svg>";
    h += "<div><span class=\"brand-name\">PatternTriggerCommand</span><span class=\"brand-sub\">Multi-Folder v3.1 &middot; " + title + "</span></div></a>\n";
    h += "  <div class=\"nav\">\n";
    h += std::string("    <a href=\"/\"") + (active == "dashboard" ? " class=\"active\"" : "") + ">" ICO_HOME "<span>Dashboard</span></a>\n";
    h += std::string("    <a href=\"/scheduler\"") + (active == "scheduler" ? " class=\"active\"" : "") + ">" ICO_CLOCK "<span>Schedulatore</span></a>\n";
    h += std::string("    <a href=\"/settings\"") + (active == "settings" ? " class=\"active\"" : "") + ">" ICO_GEAR "<span>Impostazioni</span></a>\n";
    h += "  </div>\n";
    h += "  <div class=\"live\" id=\"liveBox\" title=\"Stato connessione con il servizio\"><span class=\"dot dot-off\" id=\"liveDot\"></span><span id=\"liveText\">Connessione...</span></div>\n";
    std::string slug = title;
    std::transform(slug.begin(), slug.end(), slug.begin(), ::tolower);
    h += "</div>\n<div class=\"top-stripe\"></div>\n<div class=\"main\">\n";
    h += "<div class=\"prompt\"><b>ptc@service</b>:<i>~</i>$ " + slug + " --live</div>\n";
    return h;
}

std::string GetPageFooter() {
    return "</div>\n<div class=\"toasts\" id=\"toasts\"></div>\n"
           "<div class=\"footer\"><strong>PatternTriggerCommand</strong> Multi-Folder v3.1 &middot; Autore: Umberto Meglio &middot; Supporto alla creazione: Claude di Anthropic &middot; Composto in <a href=\"https://github.com/umeglio/miraFONT\" target=\"_blank\" rel=\"noopener\">miraFONT</a></div>\n"
           "</body>\n</html>";
}

// Funzioni JavaScript condivise da tutte le pagine (toast, escape, chiamate API, indicatore live)
std::string GetCommonJs() {
    return R"js(
function esc(s){if(s===undefined||s===null)return"";var d=document.createElement("div");d.appendChild(document.createTextNode(String(s)));return d.innerHTML;}
function toast(msg,type,ms){var box=document.getElementById("toasts");var t=document.createElement("div");t.className="toast "+(type||"");t.textContent=msg;box.appendChild(t);setTimeout(function(){t.style.opacity="0";t.style.transition="opacity 0.3s";setTimeout(function(){if(t.parentNode)box.removeChild(t)},300)},ms||3500);}
function setLive(ok,text){var d=document.getElementById("liveDot"),t=document.getElementById("liveText");if(!d)return;d.className="dot "+(ok?"dot-on":"dot-off");t.textContent=text||(ok?"In linea":"Non raggiungibile");}
function apiGet(url,cb,err){var x=new XMLHttpRequest();x.open("GET",url,true);x.timeout=8000;x.onload=function(){if(x.status===200){try{cb(JSON.parse(x.responseText));}catch(e){if(err)err("Risposta non valida");}}else{if(err)err("HTTP "+x.status);}};x.onerror=function(){if(err)err("Errore di rete");};x.ontimeout=function(){if(err)err("Timeout");};x.send();}
function apiPost(url,data,cb,err){var x=new XMLHttpRequest();x.open("POST",url,true);x.timeout=15000;x.setRequestHeader("Content-Type","application/json");x.onload=function(){if(x.status===200){try{var r=JSON.parse(x.responseText);if(r.success){if(cb)cb(r);}else{var m=r.error||"Operazione fallita";if(err)err(m);else toast(m,"err",6000);}}catch(e){if(err)err("Risposta non valida");else toast("Risposta non valida dal server","err");}}else{if(err)err("HTTP "+x.status);else toast("Errore HTTP "+x.status,"err");}};x.onerror=function(){if(err)err("Errore di rete");else toast("Errore di rete","err");};x.ontimeout=function(){if(err)err("Timeout");else toast("Timeout della richiesta","err");};x.send(JSON.stringify(data));}
function fmtTs(ts){var d=new Date(ts*1000),n=new Date();var t=d.toLocaleTimeString("it-IT");if(d.toDateString()!==n.toDateString())return d.toLocaleDateString("it-IT",{day:"2-digit",month:"2-digit"})+" "+t;return t;}
function fmtUp(s){if(s<0)return"N/A";var d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60);if(d>0)return d+"g "+h+"h "+m+"m";if(h>0)return h+"h "+m+"m";return m+"m "+(s%60)+"s";}
function fmtAgo(s){if(s<0)return"Mai";if(s<60)return s+" s fa";if(s<3600)return Math.floor(s/60)+" min fa";if(s<86400)return Math.floor(s/3600)+" h fa";return Math.floor(s/86400)+" g fa";}
)js";
}

// ====== PAGINA DASHBOARD ======

std::string GetDashboardHtml() {
    std::string html = GetPageHeader("Dashboard", "dashboard");
    html += R"html(
<div class="page-head">
    <div><h2>Dashboard</h2><p>Stato in tempo reale del servizio, delle cartelle monitorate e dei pattern configurati.</p></div>
    <div class="muted" style="font-size:0.82em" id="lastUpdate">Aggiornamento...</div>
</div>
<div id="error" class="alert alert-error" style="display:none;">)html" ICO_ALERT R"html(<div id="errorText"></div></div>
<div id="loading" class="loading">Caricamento dati...</div>
<div id="dashboard" style="display:none;">
    <div class="stats">
        <div class="stat-card"><div class="stat-icon">)html" ICO_FILE R"html(</div><div class="stat-body"><div class="stat-label">File processati</div><div class="stat-value" id="totalFiles">-</div></div></div>
        <div class="stat-card"><div class="stat-icon green">)html" ICO_CAL R"html(</div><div class="stat-body"><div class="stat-label">File oggi</div><div class="stat-value" id="todayFiles">-</div></div></div>
        <div class="stat-card"><div class="stat-icon purple">)html" ICO_TERMINAL R"html(</div><div class="stat-body"><div class="stat-label">Comandi eseguiti</div><div class="stat-value" id="commandsExecuted">-</div></div></div>
        <div class="stat-card"><div class="stat-icon red">)html" ICO_ALERT R"html(</div><div class="stat-body"><div class="stat-label">Errori</div><div class="stat-value" id="errorsCount">-</div></div></div>
        <div class="stat-card"><div class="stat-icon warn">)html" ICO_MEM R"html(</div><div class="stat-body"><div class="stat-label">Memoria</div><div class="stat-value" id="memoryUsage">-</div></div></div>
        <div class="stat-card"><div class="stat-icon">)html" ICO_CHIP R"html(</div><div class="stat-body"><div class="stat-label">Thread attivi</div><div class="stat-value" id="activeThreads">-</div></div></div>
        <div class="stat-card"><div class="stat-icon green">)html" ICO_CLOCK R"html(</div><div class="stat-body"><div class="stat-label">Uptime</div><div class="stat-value" id="uptime" style="font-size:1.1em">-</div></div></div>
        <div class="stat-card"><div class="stat-icon">)html" ICO_ACTIVITY R"html(</div><div class="stat-body"><div class="stat-label">Ultimo file</div><div class="stat-value" id="lastActivity" style="font-size:1.05em">-</div></div></div>
    </div>
    <div class="grid2">
        <div class="card">
            <div class="card-header"><span class="card-title">)html" ICO_LAYERS R"html(Stato servizio</span><a class="btn btn-ghost btn-sm" href="/settings">)html" ICO_GEAR R"html(Impostazioni</a></div>
            <table class="kv">
                <tr><td>Cartelle monitorate</td><td id="foldersCount">-</td></tr>
                <tr><td>Pattern configurati</td><td id="patternsCount">-</td></tr>
                <tr><td>Web server</td><td id="webServerStatus">-</td></tr>
                <tr><td>Schedulatore</td><td id="schedulerStatus">-</td></tr>
                <tr><td>Tempo medio elaborazione</td><td id="avgProcessing">-</td></tr>
                <tr><td>Configurazione</td><td id="reloadStatus">-</td></tr>
            </table>
        </div>
        <div class="card">
            <div class="card-header"><span class="card-title">)html" ICO_ACTIVITY R"html(Attivit&agrave; recente</span><span class="muted" style="font-size:0.8em">ultimi 20 eventi</span></div>
            <div id="recentActivity" class="activity-list"><div class="empty-state">Nessuna attivit&agrave; registrata</div></div>
        </div>
    </div>

    <div class="grid2">
        <div class="card">
            <div class="card-header"><span class="card-title">)html" ICO_ACTIVITY R"html(Andamento ultime 24 ore</span><span class="muted" style="font-size:0.76em" id="trendInfo">campionamento ogni minuto</span></div>
            <div class="chart-wrap"><canvas id="chartTrend"></canvas></div>
            <div class="chart-legend"><span><i style="background:#2fd0ff"></i>file processati / min</span><span><i style="background:#7dffb0"></i>comandi / min</span><span><i style="background:#ff5f57"></i>errori / min</span></div>
        </div>
        <div class="card">
            <div class="card-header"><span class="card-title">)html" ICO_CAL R"html(Distribuzione oraria di oggi</span><span class="muted" style="font-size:0.76em" id="hourlyInfo"></span></div>
            <div class="chart-wrap"><canvas id="chartHourly"></canvas></div>
            <div class="chart-legend"><span><i style="background:#2fd0ff"></i>file</span><span><i style="background:#7dffb0"></i>comandi</span></div>
        </div>
    </div>
    <div class="grid3">
        <div class="card">
            <div class="card-header"><span class="card-title">)html" ICO_MEM R"html(Memoria del servizio</span><span class="muted" style="font-size:0.76em" id="memInfo"></span></div>
            <div class="chart-wrap"><canvas id="chartMem"></canvas></div>
        </div>
        <div class="card">
            <div class="card-header"><span class="card-title">)html" ICO_REGEX R"html(Pattern pi&ugrave; attivi</span></div>
            <div class="bars" id="patternBars"><div class="empty-state">Nessun dato</div></div>
        </div>
        <div class="card">
            <div class="card-header"><span class="card-title">)html" ICO_CLOCK R"html(Esito schedulatore</span></div>
            <div class="donut">
                <svg viewBox="0 0 42 42" id="schedDonut"><circle cx="21" cy="21" r="15.9" fill="none" stroke="#1a2a4a" stroke-width="5"/><circle id="donutOk" cx="21" cy="21" r="15.9" fill="none" stroke="#7dffb0" stroke-width="5" stroke-dasharray="0 100" stroke-dashoffset="25" stroke-linecap="butt"/><circle id="donutFail" cx="21" cy="21" r="15.9" fill="none" stroke="#ff5f57" stroke-width="5" stroke-dasharray="0 100" stroke-dashoffset="25"/><text x="21" y="23" text-anchor="middle" font-size="7" fill="#a8ecff" font-family="miraFONT,monospace" id="donutPct">-</text></svg>
                <div class="dlist"><span><span class="dot dot-on"></span> successi<b id="schedOk">-</b></span><span><span class="dot dot-off"></span> errori<b id="schedFail">-</b></span><span>task attivi<b id="schedActive">-</b></span><span>esecuzioni totali<b id="schedTotal">-</b></span></div>
            </div>
            <div class="bars" id="taskBars" style="margin-top:14px"></div>
        </div>
    </div>
    <div class="card">
        <div class="card-header"><span class="card-title">)html" ICO_FOLDER R"html(Cartelle monitorate <span class="count" id="foldersBadge">0</span></span></div>
        <div class="table-wrap">
        <table>
            <thead><tr><th>Stato</th><th>Percorso</th><th style="text-align:right">File rilevati</th><th style="text-align:right">File processati</th></tr></thead>
            <tbody id="foldersTableBody"></tbody>
        </table>
        </div>
        <div id="foldersEmpty" class="empty-state" style="display:none;">Nessuna cartella monitorata. Aggiungi un pattern dalle <a href="/settings">impostazioni</a>.</div>
    </div>
    <div class="card">
        <div class="card-header"><span class="card-title">)html" ICO_REGEX R"html(Pattern configurati <span class="count" id="patternsBadge">0</span></span><a class="btn btn-ghost btn-sm" href="/settings#patterns">)html" ICO_EDIT R"html(Modifica pattern</a></div>
        <div class="table-wrap">
        <table>
            <thead><tr><th>Nome</th><th>Cartella</th><th>Regex</th><th>Comando</th><th style="text-align:right">Match</th><th style="text-align:right">Esecuzioni</th></tr></thead>
            <tbody id="patternsTableBody"></tbody>
        </table>
        </div>
        <div id="patternsEmpty" class="empty-state" style="display:none;">Nessun pattern configurato.</div>
    </div>
</div>
<script>)html";
    html += GetCommonJs();
    html += R"js(
var failures=0;
function badge(on,txtOn,txtOff){return"<span class='badge "+(on?"badge-on":"badge-off")+"'><span class='dot "+(on?"dot-on":"dot-off")+"'></span>"+(on?txtOn:txtOff)+"</span>";}
function update(){
    apiGet("/api/metrics",function(data){
        failures=0;setLive(true);
        document.getElementById("loading").style.display="none";
        document.getElementById("error").style.display="none";
        var wasHidden=document.getElementById("dashboard").style.display!=="block";
        document.getElementById("dashboard").style.display="block";
        if(wasHidden)setTimeout(updateStats,50);
        document.getElementById("lastUpdate").textContent="Aggiornato alle "+new Date().toLocaleTimeString("it-IT");
        document.getElementById("totalFiles").textContent=data.totalFilesProcessed;
        document.getElementById("todayFiles").textContent=data.filesProcessedToday;
        document.getElementById("commandsExecuted").textContent=data.commandsExecuted;
        var ec=document.getElementById("errorsCount");ec.textContent=data.errorsCount;ec.className="stat-value"+(data.errorsCount>0?" alert":"");
        document.getElementById("memoryUsage").textContent=data.memoryUsageMB+" MB";
        document.getElementById("activeThreads").textContent=data.activeThreads;
        document.getElementById("avgProcessing").textContent=data.averageProcessingTime+" ms";
        document.getElementById("uptime").textContent=fmtUp(data.uptimeSeconds);
        document.getElementById("lastActivity").textContent=fmtAgo(data.lastActivitySeconds);
        document.getElementById("foldersCount").textContent=data.foldersMonitored;
        document.getElementById("patternsCount").textContent=data.patternsConfigured;
        document.getElementById("foldersBadge").textContent=data.foldersMonitored;
        document.getElementById("patternsBadge").textContent=data.patternsConfigured;
        document.getElementById("webServerStatus").innerHTML=badge(data.webServerRunning,"Attivo su porta "+data.webServerPort,"Inattivo");
        document.getElementById("schedulerStatus").innerHTML=data.schedulerEnabled?badge(true,data.schedulerTasks+" task","" ):badge(false,"","Disattivato");
        document.getElementById("reloadStatus").innerHTML=data.reloadPending?"<span class='badge badge-warn'>Ricaricamento in corso...</span>":"<span class='badge badge-info'>"+esc(data.configFile)+"</span>";
        var fb=document.getElementById("foldersTableBody");fb.innerHTML="";
        document.getElementById("foldersEmpty").style.display=data.folders.length?"none":"block";
        data.folders.forEach(function(f){
            var tr=document.createElement("tr");
            tr.innerHTML="<td>"+badge(f.active,"Attivo","Fermo")+"</td><td><span class='mono'>"+esc(f.path)+"</span></td><td class='num'>"+f.filesDetected+"</td><td class='num'>"+f.filesProcessed+"</td>";
            fb.appendChild(tr);
        });
        var pb=document.getElementById("patternsTableBody");pb.innerHTML="";
        document.getElementById("patternsEmpty").style.display=data.patterns.length?"none":"block";
        data.patterns.forEach(function(p){
            var tr=document.createElement("tr");
            tr.innerHTML="<td><strong>"+esc(p.name)+"</strong></td><td>"+esc(p.folder)+"</td><td><span class='mono'>"+esc(p.regex)+"</span></td><td class='muted' style='font-size:0.85em'>"+esc(p.command)+"</td><td class='num'>"+p.matchCount+"</td><td class='num'>"+p.executionCount+"</td>";
            pb.appendChild(tr);
        });
        var ad=document.getElementById("recentActivity");ad.innerHTML="";
        if(!data.recentActivity.length){ad.innerHTML="<div class='empty-state'>Nessuna attivit\u00e0 registrata</div>";}
        data.recentActivity.slice().reverse().forEach(function(a){
            var div=document.createElement("div");var m=a.message||"";
            div.className="activity-item"+(/ERRORE|TIMEOUT|fallit/i.test(m)?" err":(/completat|avviat|successo|caricat/i.test(m)?" ok":""));
            div.innerHTML="<span class='adot'></span><span class='activity-msg'>"+esc(m)+"</span><span class='activity-time'>"+fmtTs(a.timestamp)+"</span>";
            ad.appendChild(div);
        });
    },function(msg){
        failures++;setLive(false);
        if(failures>=2){document.getElementById("loading").style.display="none";var e=document.getElementById("error");document.getElementById("errorText").textContent="Impossibile contattare il servizio ("+msg+"). Nuovo tentativo automatico tra pochi secondi.";e.style.display="flex";}
    });
}

function setupCanvas(c){var dpr=window.devicePixelRatio||1;var r=c.getBoundingClientRect();if(r.width===0)return null;c.width=Math.round(r.width*dpr);c.height=Math.round(r.height*dpr);var ctx=c.getContext("2d");ctx.setTransform(dpr,0,0,dpr,0,0);return{ctx:ctx,w:r.width,h:r.height};}
function drawLines(id,series,labels,opts){
    var c=document.getElementById(id),s=setupCanvas(c);if(!s)return;var ctx=s.ctx,W=s.w,H=s.h;opts=opts||{};
    var padL=38,padR=10,padT=10,padB=22,pw=W-padL-padR,ph=H-padT-padB;
    ctx.clearRect(0,0,W,H);
    var max=opts.max||0;series.forEach(function(sr){sr.data.forEach(function(v){if(v>max)max=v;});});if(max<=0)max=1;max=max*1.15;
    ctx.font="10px 'miraFONT Linear',monospace";ctx.fillStyle="#6a82ab";ctx.strokeStyle="rgba(47,208,255,0.12)";ctx.lineWidth=1;
    for(var g=0;g<=4;g++){var y=padT+ph-ph*g/4;ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();ctx.textAlign="right";ctx.fillText(fmtNum(max*g/4),padL-5,y+3);}
    var n=labels.length;if(n<2){ctx.textAlign="center";ctx.fillStyle="#6a82ab";ctx.fillText("In attesa di campioni...",W/2,H/2);return;}
    ctx.textAlign="center";var step=Math.max(1,Math.ceil(n/6));for(var i=0;i<n;i+=step){ctx.fillText(labels[i],padL+pw*i/(n-1),H-6);}
    series.forEach(function(sr){
        ctx.beginPath();ctx.lineWidth=2;ctx.strokeStyle=sr.color;ctx.shadowColor=sr.color;ctx.shadowBlur=8;
        sr.data.forEach(function(v,i){var x=padL+pw*i/(n-1),y=padT+ph-ph*v/max;if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});
        ctx.stroke();ctx.shadowBlur=0;
        if(sr.fill){ctx.lineTo(padL+pw,padT+ph);ctx.lineTo(padL,padT+ph);ctx.closePath();ctx.fillStyle=sr.fill;ctx.fill();}
    });
}
function drawBars(id,groups,labels){
    var c=document.getElementById(id),s=setupCanvas(c);if(!s)return;var ctx=s.ctx,W=s.w,H=s.h;
    var padL=32,padR=6,padT=10,padB=22,pw=W-padL-padR,ph=H-padT-padB;ctx.clearRect(0,0,W,H);
    var max=1;groups.forEach(function(g){g.data.forEach(function(v){if(v>max)max=v;});});max=max*1.15;
    ctx.font="10px 'miraFONT Linear',monospace";ctx.fillStyle="#6a82ab";ctx.strokeStyle="rgba(47,208,255,0.12)";
    for(var g=0;g<=4;g++){var y=padT+ph-ph*g/4;ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();ctx.textAlign="right";ctx.fillText(fmtNum(max*g/4),padL-4,y+3);}
    var n=labels.length,slot=pw/n,bw=Math.max(2,(slot-4)/groups.length);
    ctx.textAlign="center";labels.forEach(function(l,i){if(i%3===0)ctx.fillText(l,padL+slot*i+slot/2,H-6);});
    groups.forEach(function(gr,gi){ctx.fillStyle=gr.color;ctx.shadowColor=gr.color;ctx.shadowBlur=6;gr.data.forEach(function(v,i){var h=ph*v/max;if(h<1&&v>0)h=1;ctx.fillRect(padL+slot*i+2+bw*gi,padT+ph-h,bw-1,h);});ctx.shadowBlur=0;});
}
function fmtNum(v){if(v>=1000000)return(v/1000000).toFixed(1)+"M";if(v>=1000)return(v/1000).toFixed(1)+"k";if(v%1!==0)return v.toFixed(1);return String(v);}
function barRows(el,items,cls,emptyMsg){
    if(!items.length){el.innerHTML="<div class='empty-state' style='padding:14px'>"+esc(emptyMsg||"Nessun dato")+"</div>";return;}
    var max=Math.max.apply(null,items.map(function(i){return i.value}))||1;
    el.innerHTML=items.map(function(i){return"<div class='bar-row'><span class='bar-label' title='"+esc(i.label)+"'>"+esc(i.label)+"</span><div class='bar-track'><div class='bar-fill "+(cls||"")+"' style='width:"+(100*i.value/max)+"%'></div></div><span class='bar-val'>"+i.value+"</span></div>"}).join("");
}
function updateStats(){
    if(document.getElementById("dashboard").style.display!=="block"){setTimeout(updateStats,1000);return;}
    apiGet("/api/stats",function(st){
        var sm=st.samples||[];var labels=[],files=[],cmds=[],errs=[],mems=[];
        for(var i=1;i<sm.length;i++){var a=sm[i-1],b=sm[i];var d=new Date(b.t*1000);labels.push(d.getHours().toString().padStart(2,"0")+":"+d.getMinutes().toString().padStart(2,"0"));files.push(Math.max(0,b.f-a.f));cmds.push(Math.max(0,b.c-a.c));errs.push(Math.max(0,b.e-a.e));mems.push(b.m);}
        if(sm.length===1){var d0=new Date(sm[0].t*1000);labels.push(d0.getHours().toString().padStart(2,"0")+":"+d0.getMinutes().toString().padStart(2,"0"));mems.push(sm[0].m);}
        drawLines("chartTrend",[{data:files,color:"#2fd0ff",fill:"rgba(47,208,255,0.08)"},{data:cmds,color:"#7dffb0"},{data:errs,color:"#ff5f57"}],labels);
        document.getElementById("trendInfo").textContent=sm.length+" campioni · media "+fmtNum(st.filesPerHour)+" file/ora";
        drawLines("chartMem",[{data:mems,color:"#ffcf6b",fill:"rgba(255,207,107,0.08)"}],labels,{});
        var curMem=mems.length?mems[mems.length-1]:0,maxMem=mems.length?Math.max.apply(null,mems):0;
        document.getElementById("memInfo").textContent=curMem+" MB ora · picco "+maxMem+" MB";
        var hl=[];for(var h=0;h<24;h++)hl.push(h.toString().padStart(2,"0"));
        drawBars("chartHourly",[{data:st.hourlyFiles,color:"#2fd0ff"},{data:st.hourlyCommands,color:"#7dffb0"}],hl);
        var peakH=0,peakV=-1;st.hourlyFiles.forEach(function(v,h){if(v>peakV){peakV=v;peakH=h;}});
        document.getElementById("hourlyInfo").textContent=st.filesProcessedToday+" file oggi"+(peakV>0?" · picco ore "+hl[peakH]:"");
        var pats=(st.patterns||[]).slice().sort(function(a,b){return b.exec-a.exec}).slice(0,8).map(function(p){return{label:p.name+" ("+p.match+" match)",value:p.exec}});
        barRows(document.getElementById("patternBars"),pats,"","Nessun pattern eseguito finora");
        var sc=st.scheduler||{ok:0,fail:0};var tot=sc.ok+sc.fail;var pctOk=tot?Math.round(100*sc.ok/tot):0;
        document.getElementById("donutOk").setAttribute("stroke-dasharray",(tot?100*sc.ok/tot:0)+" 100");
        document.getElementById("donutFail").setAttribute("stroke-dasharray",(tot?100*sc.fail/tot:0)+" 100");
        document.getElementById("donutFail").setAttribute("stroke-dashoffset",String(25-(tot?100*sc.ok/tot:0)));
        document.getElementById("donutPct").textContent=tot?pctOk+"%":"n/d";
        document.getElementById("schedOk").textContent=sc.ok;document.getElementById("schedFail").textContent=sc.fail;
        document.getElementById("schedActive").textContent=sc.enabledTasks+"/"+sc.tasks;document.getElementById("schedTotal").textContent=tot;
        barRows(document.getElementById("taskBars"),(sc.topTasks||[]).filter(function(t){return t.exec>0}).slice(0,5).map(function(t){return{label:t.name,value:t.exec}}),"green","");
    });
}
updateStats();setInterval(updateStats,15000);window.addEventListener("resize",function(){clearTimeout(window.__rs);window.__rs=setTimeout(updateStats,250);});
update();setInterval(update,2000);
)js";
    html += "</script>\n" + GetPageFooter();
    return html;
}

// ====== PAGINA SCHEDULATORE ======

std::string GetSchedulerPageHtml() {
    std::string html = GetPageHeader("Schedulatore", "scheduler");
    html += R"html(
<div class="page-head">
    <div><h2>Schedulatore</h2><p>Esecuzione pianificata di script e comandi: per giorno, ora e minuto oppure a intervalli regolari.</p></div>
    <button class="btn btn-primary" onclick="openForm()">)html" ICO_PLUS R"html(Nuovo task</button>
</div>
<div id="schedOff" class="alert alert-warn" style="display:none;">)html" ICO_ALERT R"html(<div>Lo schedulatore &egrave; <strong>disattivato</strong> nella configurazione: i task non verranno eseguiti. Puoi riattivarlo dalle <a href="/settings">impostazioni</a>.</div></div>
<div class="stats">
    <div class="stat-card"><div class="stat-icon green">)html" ICO_PLAY R"html(</div><div class="stat-body"><div class="stat-label">Stato</div><div class="stat-value" id="sStatus">-</div></div></div>
    <div class="stat-card"><div class="stat-icon">)html" ICO_LAYERS R"html(</div><div class="stat-body"><div class="stat-label">Task totali</div><div class="stat-value" id="sTotal">-</div></div></div>
    <div class="stat-card"><div class="stat-icon green">)html" ICO_CHECK R"html(</div><div class="stat-body"><div class="stat-label">Task attivi</div><div class="stat-value" id="sActive">-</div></div></div>
    <div class="stat-card"><div class="stat-icon purple">)html" ICO_TERMINAL R"html(</div><div class="stat-body"><div class="stat-label">Esecuzioni totali</div><div class="stat-value" id="sExecs">-</div></div></div>
    <div class="stat-card"><div class="stat-icon warn">)html" ICO_FOLDER R"html(</div><div class="stat-body"><div class="stat-label">Cartella task</div><div class="stat-value small" id="sFolder">-</div></div></div>
</div>
<div class="tabs">
    <button class="tab active" onclick="switchTab('tasks',this)">Task</button>
    <button class="tab" onclick="switchTab('history',this)">Storico esecuzioni</button>
</div>
<div id="panel-tasks" class="panel active">
    <div class="card">
        <div class="card-header"><span class="card-title">)html" ICO_CAL R"html(Task schedulati <span class="count" id="tCount">0</span></span></div>
        <div class="table-wrap">
        <table>
            <thead><tr><th>Nome</th><th>Stato</th><th>Tipo</th><th>Programmazione</th><th>Comando</th><th>Ultima esec.</th><th style="text-align:right">#</th><th>Azioni</th></tr></thead>
            <tbody id="tBody"></tbody>
        </table>
        </div>
        <div id="tEmpty" class="empty-state" style="display:none;">Nessun task configurato. Crea il primo con il pulsante <strong>Nuovo task</strong>.</div>
    </div>
</div>
<div id="panel-history" class="panel">
    <div class="card">
        <div class="card-header"><span class="card-title">)html" ICO_ACTIVITY R"html(Storico esecuzioni <span class="count" id="hCount">0</span></span></div>
        <div class="filters">
            <input type="text" id="hFilter" placeholder="Filtra per nome task..." oninput="renderHistory()">
            <select id="hStatus" onchange="renderHistory()"><option value="">Tutti gli esiti</option><option value="ok">Solo successi</option><option value="fail">Solo errori</option></select>
        </div>
        <div class="table-wrap">
        <table>
            <thead><tr><th>Data/ora</th><th>Task</th><th>Comando</th><th>Esito</th><th style="text-align:right">Codice</th></tr></thead>
            <tbody id="hBody"></tbody>
        </table>
        </div>
        <div id="hEmpty" class="empty-state" style="display:none;">Nessuna esecuzione registrata.</div>
    </div>
</div>
<div id="formOverlay" class="overlay" style="display:none;" onclick="if(event.target===this)closeForm()">
    <div class="modal">
        <h2 id="fTitle">Nuovo task</h2>
        <input type="hidden" id="fOrig" value="">
        <div class="field">
            <label>Nome task</label>
            <input type="text" id="fName" placeholder="Es: Backup giornaliero">
        </div>
        <div class="field">
            <label>Modalit&agrave; di schedulazione</label>
            <div class="mode-switch">
                <button class="mode-btn active" onclick="setMode('schedule',this)">Giorno / ora / minuto</button>
                <button class="mode-btn" onclick="setMode('interval',this)">Intervallo (ogni N secondi)</button>
            </div>
        </div>
        <div id="modeSchedule" class="mode-section active">
            <div class="field">
                <label>Giorni della settimana</label>
                <div class="days-row" id="fDays"></div>
            </div>
            <div class="form-grid">
                <div class="field">
                    <label>Ore</label>
                    <input type="text" id="fHours" placeholder="Es: 0,6,12,18">
                    <div class="hint">Ore separate da virgola (0-23)</div>
                </div>
                <div class="field">
                    <label>Minuti</label>
                    <input type="text" id="fMinutes" placeholder="Es: 0,15,30,45">
                    <div class="hint">Minuti separati da virgola (0-59)</div>
                </div>
            </div>
        </div>
        <div id="modeInterval" class="mode-section">
            <div class="field">
                <label>Ripeti ogni (secondi)</label>
                <input type="number" id="fInterval" min="5" value="60" placeholder="60">
                <div class="hint">Minimo 5 secondi. Es: 60 = ogni minuto, 3600 = ogni ora</div>
            </div>
        </div>
        <div class="field">
            <label>Comando da eseguire</label>
            <select id="fScriptSelect" onchange="if(this.value)document.getElementById('fCommand').value=this.value">
                <option value="">-- Seleziona da elenco --</option>
            </select>
            <input type="text" id="fCommand" placeholder="C:\Scripts\myscript.bat" style="margin-top:8px;">
            <div class="hint">Seleziona un file dall'elenco (C:\Scripts e cartella task) oppure scrivi il percorso completo</div>
        </div>
        <div class="modal-actions">
            <button class="btn btn-ghost" onclick="closeForm()">Annulla</button>
            <button class="btn btn-primary" onclick="saveTask()">)html" ICO_SAVE R"html(Salva task</button>
        </div>
    </div>
</div>
<script>)html";
    html += GetCommonJs();
    html += R"js(
var T=[],H=[],scripts=[];
var curMode="schedule";
var DAYS=["Lu","Ma","Me","Gi","Ve","Sa","Do"];
function switchTab(id,el){document.querySelectorAll(".tab").forEach(function(t){t.classList.remove("active")});el.classList.add("active");document.querySelectorAll(".panel").forEach(function(p){p.classList.remove("active")});document.getElementById("panel-"+id).classList.add("active");}
function setMode(m,el){curMode=m;document.querySelectorAll(".mode-btn").forEach(function(b){b.classList.remove("active")});el.classList.add("active");document.getElementById("modeSchedule").className=m==="schedule"?"mode-section active":"mode-section";document.getElementById("modeInterval").className=m==="interval"?"mode-section active":"mode-section";}
function initDays(){var c=document.getElementById("fDays");c.innerHTML="";DAYS.forEach(function(d){var chip=document.createElement("div");chip.className="day-chip";chip.textContent=d;chip.setAttribute("data-day",d);chip.onclick=function(){this.classList.toggle("on")};c.appendChild(chip)});}
function loadData(){
    apiGet("/api/scheduler",function(data){
        setLive(true);
        T=data.tasks||[];H=data.history||[];
        var el=document.getElementById("sStatus");el.textContent=data.enabled?"Attivo":"Disattivo";el.className="stat-value "+(data.enabled?"on":"off");
        document.getElementById("schedOff").style.display=data.enabled?"none":"flex";
        document.getElementById("sTotal").textContent=T.length;
        document.getElementById("sActive").textContent=T.filter(function(t){return t.enabled}).length;
        var totalExec=0;T.forEach(function(t){totalExec+=t.executionCount});document.getElementById("sExecs").textContent=totalExec;
        document.getElementById("sFolder").textContent=data.folder;
        renderTasks();renderHistory();
    },function(){setLive(false);});
    apiGet("/api/scheduler/scripts",function(data){
        scripts=data||[];
        var sel=document.getElementById("fScriptSelect");
        var curr=sel.value;
        sel.innerHTML="<option value=''>-- Seleziona da elenco ("+scripts.length+" file) --</option>";
        scripts.forEach(function(s){var o=document.createElement("option");o.value=s;o.textContent=s;sel.appendChild(o)});
        if(curr)sel.value=curr;
    });
}
function renderTasks(){
    var tb=document.getElementById("tBody");tb.innerHTML="";
    document.getElementById("tEmpty").style.display=T.length?"none":"block";
    document.getElementById("tCount").textContent=T.length;
    T.forEach(function(t,i){
        var tr=document.createElement("tr");
        var isInt=t.intervalSeconds>0;
        var sched=isInt?("Ogni "+fmtInterval(t.intervalSeconds)):(t.days+" &middot; ore "+esc(t.hours)+" &middot; min "+esc(t.minutes));
        var typeB=isInt?"<span class='badge badge-interval'>Intervallo</span>":"<span class='badge badge-schedule'>Programmato</span>";
        tr.innerHTML="<td><strong>"+esc(t.name)+"</strong></td>"
            +"<td><span class='badge "+(t.enabled?"badge-on":"badge-off")+"'><span class='dot "+(t.enabled?"dot-on":"dot-off")+"'></span>"+(t.enabled?"Attivo":"Fermo")+"</span></td>"
            +"<td>"+typeB+"</td>"
            +"<td style='font-size:0.85em'>"+sched+"</td>"
            +"<td><span class='mono'>"+esc(t.command)+"</span></td>"
            +"<td style='font-size:0.85em;white-space:nowrap'>"+(t.lastExecution?esc(t.lastExecution):"<span class='muted'>Mai</span>")+"</td>"
            +"<td class='num'>"+t.executionCount+"</td>"
            +"<td><div class='actions'>"
            +"<button class='btn btn-sm "+(t.enabled?"btn-warning":"btn-success")+"' data-i='"+i+"' data-a='tog' title='"+(t.enabled?"Disattiva":"Attiva")+"'>"+(t.enabled?"Stop":"Avvia")+"</button>"
            +"<button class='btn btn-sm btn-ghost' data-i='"+i+"' data-a='edit' title='Modifica'>Modifica</button>"
            +"<button class='btn btn-sm btn-ghost' data-i='"+i+"' data-a='dup' title='Duplica'>Duplica</button>"
            +"<button class='btn btn-sm btn-danger' data-i='"+i+"' data-a='del' title='Elimina'>Elimina</button>"
            +"</div></td>";
        tb.appendChild(tr);
    });
}
document.getElementById("tBody").addEventListener("click",function(ev){
    var b=ev.target.closest("button[data-a]");if(!b)return;
    var t=T[parseInt(b.getAttribute("data-i"))];if(!t)return;
    var a=b.getAttribute("data-a");
    if(a==="tog")togTask(t);else if(a==="edit")openForm(t);else if(a==="dup")dupeTask(t);else if(a==="del")delTask(t);
});
function renderHistory(){
    var filter=(document.getElementById("hFilter").value||"").toLowerCase();
    var status=document.getElementById("hStatus").value;
    var filtered=H.filter(function(e){
        if(filter&&e.taskName.toLowerCase().indexOf(filter)<0)return false;
        if(status==="ok"&&!e.success)return false;
        if(status==="fail"&&e.success)return false;
        return true;
    });
    document.getElementById("hCount").textContent=H.length;
    var tb=document.getElementById("hBody");tb.innerHTML="";
    document.getElementById("hEmpty").style.display=filtered.length?"none":"block";
    filtered.slice().reverse().forEach(function(e){
        var tr=document.createElement("tr");
        tr.innerHTML="<td style='white-space:nowrap'>"+esc(e.timestamp)+"</td>"
            +"<td><strong>"+esc(e.taskName)+"</strong></td>"
            +"<td><span class='mono'>"+esc(e.command)+"</span></td>"
            +"<td><span class='badge "+(e.success?"badge-on":"badge-off")+"'>"+(e.success?"OK":"Errore")+"</span></td>"
            +"<td class='num'>"+e.exitCode+"</td>";
        tb.appendChild(tr);
    });
}
function openForm(task){
    document.getElementById("fTitle").textContent=task?(task.__dup?"Duplica task":"Modifica task"):"Nuovo task";
    document.getElementById("fOrig").value=(task&&!task.__dup)?task.name:"";
    document.getElementById("fName").value=task?task.name:"";
    document.getElementById("fCommand").value=task?task.command:"";
    document.getElementById("fScriptSelect").value=task?task.command:"";
    var isInt=task&&task.intervalSeconds>0;
    document.getElementById("fHours").value=(task&&!isInt)?task.hours:"";
    document.getElementById("fMinutes").value=(task&&!isInt)?task.minutes:"";
    document.getElementById("fInterval").value=isInt?task.intervalSeconds:60;
    curMode=isInt?"interval":"schedule";
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
    setTimeout(function(){document.getElementById("fName").focus()},50);
}
function closeForm(){document.getElementById("formOverlay").style.display="none";}
function saveTask(){
    var name=document.getElementById("fName").value.trim();
    var cmd=document.getElementById("fCommand").value.trim();
    if(!name){toast("Inserisci un nome per il task","warn");return;}
    if(!cmd){toast("Inserisci un comando da eseguire","warn");return;}
    var data={originalName:document.getElementById("fOrig").value,name:name,command:cmd,enabled:"true",intervalSeconds:"0"};
    if(curMode==="interval"){
        var iv=parseInt(document.getElementById("fInterval").value)||0;
        if(iv<5){toast("Intervallo minimo: 5 secondi","warn");return;}
        data.intervalSeconds=String(iv);
        data.days="";data.hours="";data.minutes="";
    }else{
        var days=[];document.querySelectorAll("#fDays .day-chip.on").forEach(function(c){days.push(c.getAttribute("data-day"))});
        if(!days.length){toast("Seleziona almeno un giorno","warn");return;}
        data.days=days.join(",");
        data.hours=document.getElementById("fHours").value;
        data.minutes=document.getElementById("fMinutes").value;
        if(!data.hours){toast("Inserisci almeno un'ora","warn");return;}
        if(!data.minutes){toast("Inserisci almeno un minuto","warn");return;}
    }
    apiPost("/api/scheduler/save",data,function(){closeForm();toast("Task '"+name+"' salvato","ok");loadData();});
}
function dupeTask(t){var c={};for(var k in t)c[k]=t[k];c.name=t.name+" (copia)";c.__dup=true;openForm(c);}
function togTask(t){apiPost("/api/scheduler/toggle",{name:t.name},function(r){toast("Task '"+t.name+"' "+(r.enabled?"attivato":"disattivato"),"ok");loadData()});}
function delTask(t){if(!confirm("Eliminare il task '"+t.name+"'?"))return;apiPost("/api/scheduler/delete",{name:t.name},function(){toast("Task eliminato","ok");loadData()});}
function fmtInterval(s){if(s>=86400)return Math.floor(s/86400)+"g "+Math.floor((s%86400)/3600)+"h";if(s>=3600)return Math.floor(s/3600)+"h "+Math.floor((s%3600)/60)+"m";if(s>=60)return Math.floor(s/60)+"m "+s%60+"s";return s+"s";}
document.addEventListener("keydown",function(e){if(e.key==="Escape")closeForm();});
initDays();loadData();setInterval(loadData,5000);
)js";
    html += "</script>\n" + GetPageFooter();
    return html;
}

// ====== PAGINA IMPOSTAZIONI ======

std::string GetSettingsPageHtml() {
    std::string html = GetPageHeader("Impostazioni", "settings");
    html += R"html(
<div class="page-head">
    <div><h2>Impostazioni del servizio</h2><p>Modifica la configurazione (<span class="mono" id="cfgPath">config.ini</span>) direttamente da qui: le modifiche vengono salvate su disco e applicate a caldo senza riavviare il servizio.</p></div>
    <button class="btn btn-ghost" onclick="reloadFromDisk()" title="Rilegge config.ini dal disco e riavvia i monitor">)html" ICO_REFRESH R"html(Ricarica da disco</button>
</div>
<div id="err" class="alert alert-error" style="display:none;">)html" ICO_ALERT R"html(<div id="errText"></div></div>
<div id="portBanner" class="alert alert-info" style="display:none;">)html" ICO_INFO R"html(<div id="portBannerText"></div></div>
<div id="lastErr" class="alert alert-warn" style="display:none;">)html" ICO_ALERT R"html(<div id="lastErrText"></div></div>
<div id="loading" class="loading">Caricamento configurazione...</div>
<div id="page" style="display:none;">
<div class="stats">
    <div class="stat-card"><div class="stat-icon">)html" ICO_GLOBE R"html(</div><div class="stat-body"><div class="stat-label">Web server</div><div class="stat-value" id="sPort">-</div></div></div>
    <div class="stat-card"><div class="stat-icon purple">)html" ICO_REGEX R"html(</div><div class="stat-body"><div class="stat-label">Pattern attivi</div><div class="stat-value" id="sPatterns">-</div></div></div>
    <div class="stat-card"><div class="stat-icon green">)html" ICO_CLOCK R"html(</div><div class="stat-body"><div class="stat-label">Schedulatore</div><div class="stat-value" id="sSched">-</div></div></div>
    <div class="stat-card"><div class="stat-icon warn">)html" ICO_REFRESH R"html(</div><div class="stat-body"><div class="stat-label">Ultimo ricaricamento</div><div class="stat-value small" id="sReload">-</div></div></div>
</div>
<div class="grid2">
    <div class="card">
        <div class="card-header"><span class="card-title">)html" ICO_FOLDER R"html(Percorsi</span></div>
        <div class="field"><label>Cartella monitorata predefinita</label><input type="text" id="cDefaultFolder" placeholder="C:\Monitored"><div class="hint">Usata dai pattern che non indicano una cartella</div></div>
        <div class="field"><label>Cartella task schedulatore</label><input type="text" id="cSchedFolder" placeholder="C:\PTC\schedules"><div class="hint">Contiene i file .sch dei task</div></div>
        <div class="field"><label>File di log</label><input type="text" id="cLogFile" placeholder="C:\PTC\PatternTriggerCommand.log"></div>
        <div class="field"><label>File di log dettagliato</label><input type="text" id="cDetailedLog" placeholder="C:\PTC\PatternTriggerCommand_detailed.log"></div>
        <div class="field"><label>Database file processati</label><input type="text" id="cProcessedDb" placeholder="C:\PTC\PatternTriggerCommand_processed.txt"><div class="hint">Elenco dei file gi&agrave; elaborati, per evitare duplicati</div></div>
    </div>
    <div class="card">
        <div class="card-header"><span class="card-title">)html" ICO_GEAR R"html(Servizio</span></div>
        <div class="switch"><div class="sw-text"><div class="sw-title">Log dettagliato</div><div class="sw-desc">Scrive anche gli eventi di dettaglio nel file di log dettagliato</div></div><label class="toggle"><input type="checkbox" id="cDetailedLogging"><span></span></label></div>
        <div class="switch"><div class="sw-text"><div class="sw-title">Schedulatore attivo</div><div class="sw-desc">Esegue i task pianificati nella cartella dei task</div></div><label class="toggle"><input type="checkbox" id="cSchedEnabled"><span></span></label></div>
        <div class="switch"><div class="sw-text"><div class="sw-title">Web server attivo</div><div class="sw-desc">Attenzione: se lo disattivi, questa dashboard non sar&agrave; pi&ugrave; raggiungibile e potrai riattivarla solo modificando config.ini</div></div><label class="toggle"><input type="checkbox" id="cWebEnabled"><span></span></label></div>
        <div class="field"><label>Porta web server</label><input type="number" id="cWebPort" min="1" max="65535" placeholder="8080"><div class="hint">Cambiando porta il web server viene riavviato automaticamente sul nuovo indirizzo</div></div>
        <div class="alert alert-info" style="margin:6px 0 0"><div style="font-size:0.85em">Le modifiche a cartelle e pattern riavviano i monitor delle cartelle; i file gi&agrave; presenti nelle cartelle vengono riesaminati ma quelli gi&agrave; elaborati non vengono ripetuti.</div></div>
    </div>
</div>
<div class="card" id="patterns">
    <div class="card-header">
        <span class="card-title">)html" ICO_REGEX R"html(Pattern e comandi <span class="count" id="pCount">0</span></span>
        <div class="actions">
            <input type="text" class="input" id="testName" placeholder="Prova un nome file, es: invoice_2024.pdf" style="width:280px" oninput="testPatterns()">
            <button class="btn btn-primary btn-sm" onclick="addPattern()">)html" ICO_PLUS R"html(Aggiungi pattern</button>
        </div>
    </div>
    <p class="card-desc">Ogni riga associa una cartella, un'espressione regolare (case-insensitive, deve corrispondere all'intero nome file) e il comando da eseguire con il file come parametro. Lascia vuota la cartella per usare quella predefinita.</p>
    <div class="table-wrap">
    <table class="pattern-table">
        <thead><tr><th style="width:120px">Nome</th><th>Cartella</th><th>Regex</th><th>Comando</th><th style="width:40px"></th></tr></thead>
        <tbody id="pBody"></tbody>
    </table>
    </div>
    <div id="pEmpty" class="empty-state" style="display:none;">Nessun pattern. Aggiungine uno per iniziare a monitorare una cartella.</div>
    <datalist id="scriptList"></datalist>
</div>
<div class="save-bar">
    <div class="status" id="dirtyStatus">)html" ICO_CHECK R"html(Nessuna modifica in sospeso</div>
    <div class="actions">
        <button class="btn btn-ghost" onclick="loadConfig(true)">Annulla modifiche</button>
        <button class="btn btn-success" id="saveBtn" onclick="saveConfig()">)html" ICO_SAVE R"html(Salva e applica</button>
    </div>
</div>
</div>
<script>)html";
    html += GetCommonJs();
    html += R"js(
var CFG=null,dirty=false,P=[],saving=false;
function markDirty(){if(saving)return;dirty=true;var s=document.getElementById("dirtyStatus");s.className="status dirty";s.innerHTML="&#9679; Modifiche non salvate";}
function clearDirty(){dirty=false;var s=document.getElementById("dirtyStatus");s.className="status";s.innerHTML=")js" ICO_CHECK R"js(Nessuna modifica in sospeso";}
function v(id){return document.getElementById(id).value;}
function sv(id,val){document.getElementById(id).value=val||"";}
function ck(id){return document.getElementById(id).checked;}
function sck(id,val){document.getElementById(id).checked=!!val;}
function fillForm(c){
    CFG=c;
    document.getElementById("cfgPath").textContent=c.configFile;
    sv("cDefaultFolder",c.defaultMonitoredFolder);sv("cSchedFolder",c.schedulerFolder);sv("cLogFile",c.logFile);sv("cDetailedLog",c.detailedLogFile);sv("cProcessedDb",c.processedFilesDb);
    sck("cDetailedLogging",c.detailedLogging);sck("cSchedEnabled",c.schedulerEnabled);sck("cWebEnabled",c.webServerEnabled);sv("cWebPort",c.webServerPort);
    P=(c.patterns||[]).map(function(p){return{name:p.name,folder:p.folder,regex:p.regex,command:p.command}});
    renderPatterns();
    document.getElementById("sPort").textContent=c.webServerEnabled?"porta "+c.webServerPort:"disattivo";
    document.getElementById("sPatterns").textContent=P.length;
    var ss=document.getElementById("sSched");ss.textContent=c.schedulerEnabled?"Attivo":"Disattivo";ss.className="stat-value "+(c.schedulerEnabled?"on":"off");
    document.getElementById("sReload").textContent=c.reloadPending?"in corso...":(c.lastReload||"all'avvio del servizio");
    var le=document.getElementById("lastErr");if(c.lastError){document.getElementById("lastErrText").textContent="Ultimo ricaricamento con avvisi: "+c.lastError;le.style.display="flex";}else le.style.display="none";
    clearDirty();
}
function loadConfig(silent){
    apiGet("/api/config",function(c){
        setLive(true);
        document.getElementById("loading").style.display="none";document.getElementById("err").style.display="none";document.getElementById("page").style.display="block";
        fillForm(c);if(silent===true)toast("Modifiche annullate","");
    },function(m){setLive(false);document.getElementById("loading").style.display="none";var e=document.getElementById("err");document.getElementById("errText").textContent="Impossibile leggere la configurazione: "+m;e.style.display="flex";});
    apiGet("/api/scheduler/scripts",function(list){var dl=document.getElementById("scriptList");dl.innerHTML="";(list||[]).forEach(function(s){var o=document.createElement("option");o.value=s;dl.appendChild(o)});});
}
function renderPatterns(){
    var tb=document.getElementById("pBody");tb.innerHTML="";
    document.getElementById("pEmpty").style.display=P.length?"none":"block";
    document.getElementById("pCount").textContent=P.length;
    P.forEach(function(p,i){
        var tr=document.createElement("tr");tr.setAttribute("data-i",i);
        tr.innerHTML="<td><input type='text' data-f='name' value='"+esc(p.name)+"' placeholder='Pattern"+(i+1)+"'></td>"
            +"<td><input type='text' data-f='folder' value='"+esc(p.folder)+"' placeholder='"+esc(v("cDefaultFolder")||"(cartella predefinita)")+"'></td>"
            +"<td><input type='text' class='mono-in' data-f='regex' value='"+esc(p.regex)+"' placeholder='^invoice.*\\.pdf$'></td>"
            +"<td><input type='text' data-f='command' list='scriptList' value='"+esc(p.command)+"' placeholder='C:\\Scripts\\script.bat'></td>"
            +"<td><button class='btn btn-danger btn-icon' title='Rimuovi pattern' data-del='"+i+"'>)js" ICO_TRASH R"js(</button></td>";
        tb.appendChild(tr);
    });
    testPatterns();
}
document.getElementById("pBody").addEventListener("input",function(ev){var inp=ev.target;var tr=inp.closest("tr");if(!tr)return;var i=parseInt(tr.getAttribute("data-i"));P[i][inp.getAttribute("data-f")]=inp.value;markDirty();if(inp.getAttribute("data-f")==="regex")testPatterns();});
document.getElementById("pBody").addEventListener("click",function(ev){var b=ev.target.closest("button[data-del]");if(!b)return;P.splice(parseInt(b.getAttribute("data-del")),1);markDirty();renderPatterns();});
["cDefaultFolder","cSchedFolder","cLogFile","cDetailedLog","cProcessedDb","cWebPort"].forEach(function(id){document.getElementById(id).addEventListener("input",markDirty);});
["cDetailedLogging","cSchedEnabled","cWebEnabled"].forEach(function(id){document.getElementById(id).addEventListener("change",markDirty);});
document.getElementById("cWebEnabled").addEventListener("change",function(){if(!this.checked)toast("Attenzione: disattivando il web server la dashboard non sar\u00e0 pi\u00f9 raggiungibile","warn",6000);});
function addPattern(){var n=1;while(P.some(function(p){return p.name==="Pattern"+n}))n++;P.push({name:"Pattern"+n,folder:"",regex:"",command:""});markDirty();renderPatterns();var rows=document.querySelectorAll("#pBody tr");var last=rows[rows.length-1];if(last){last.querySelector("input[data-f=regex]").focus();last.scrollIntoView({block:"nearest"});}}
function testPatterns(){
    var name=v("testName");
    document.querySelectorAll("#pBody tr").forEach(function(tr){
        var inp=tr.querySelector("input[data-f=regex]");inp.classList.remove("bad","hit");
        if(!inp.value)return;
        try{var re=new RegExp("^(?:"+inp.value+")$","i");if(name&&re.test(name))inp.classList.add("hit");}catch(e){inp.classList.add("bad");}
    });
}
function saveConfig(){
    var port=parseInt(v("cWebPort"))||0;
    if(port<1||port>65535){toast("Porta web non valida (1-65535)","err");return;}
    if(!v("cDefaultFolder").trim()){toast("Indica la cartella monitorata predefinita","err");return;}
    for(var i=0;i<P.length;i++){
        var p=P[i];
        if(!p.regex.trim()){toast("Il pattern "+(i+1)+" non ha una regex","err");return;}
        if(!p.command.trim()){toast("Il pattern "+(i+1)+" non ha un comando","err");return;}
        try{new RegExp(p.regex);}catch(e){toast("Regex non valida nel pattern "+(i+1)+": "+e.message,"err",6000);return;}
        if(p.regex.indexOf("|")>=0&&!p.folder.trim()){toast("Il pattern "+(i+1)+" usa '|' nella regex: indica esplicitamente la cartella","err",6000);return;}
    }
    if(!ck("cWebEnabled")&&!confirm("Stai per DISATTIVARE il web server: la dashboard non sar\u00e0 pi\u00f9 raggiungibile finch\u00e9 non riattivi WebServerEnabled in config.ini. Continuare?"))return;
    var lines=P.map(function(p){return[p.name.trim(),p.folder.trim(),p.regex.trim(),p.command.trim()].join("\t")}).join("\n");
    var data={defaultMonitoredFolder:v("cDefaultFolder").trim(),schedulerFolder:v("cSchedFolder").trim(),logFile:v("cLogFile").trim(),detailedLogFile:v("cDetailedLog").trim(),processedFilesDb:v("cProcessedDb").trim(),detailedLogging:ck("cDetailedLogging")?"true":"false",schedulerEnabled:ck("cSchedEnabled")?"true":"false",webServerEnabled:ck("cWebEnabled")?"true":"false",webServerPort:String(port),patterns:lines};
    saving=true;var btn=document.getElementById("saveBtn");btn.disabled=true;
    apiPost("/api/config/save",data,function(r){
        saving=false;btn.disabled=false;clearDirty();
        toast("Configurazione salvata: applicazione in corso...","ok");
        if(r.webRestart){
            var nb=document.getElementById("portBanner");
            if(r.webEnabled){var url=location.protocol+"//"+location.hostname+":"+r.newPort+"/settings";document.getElementById("portBannerText").innerHTML="Il web server viene riavviato sulla porta <strong>"+r.newPort+"</strong>. Tra qualche secondo apri <a href='"+url+"'>"+url+"</a>.";}
            else document.getElementById("portBannerText").textContent="Il web server verr\u00e0 spento: questa pagina non sar\u00e0 pi\u00f9 raggiungibile.";
            nb.style.display="flex";
        }else{
            waitReload(0);
        }
    },function(m){saving=false;btn.disabled=false;toast("Salvataggio fallito: "+m,"err",7000);});
}
function waitReload(n){
    setTimeout(function(){
        apiGet("/api/config",function(c){
            if(c.reloadPending&&n<20){waitReload(n+1);return;}
            fillForm(c);toast(c.lastError?"Configurazione applicata con avvisi":"Configurazione applicata","ok");
        },function(){if(n<20)waitReload(n+1);});
    },1000);
}
function reloadFromDisk(){
    if(dirty&&!confirm("Ci sono modifiche non salvate: ricaricando da disco andranno perse. Continuare?"))return;
    apiPost("/api/config/reload",{},function(){toast("Ricaricamento configurazione avviato","");waitReload(0);});
}
window.addEventListener("beforeunload",function(e){if(dirty){e.preventDefault();e.returnValue="";}});
loadConfig();
if(location.hash==="#patterns")setTimeout(function(){var el=document.getElementById("patterns");if(el)el.scrollIntoView({behavior:"smooth"});},300);
)js";
    html += "</script>\n" + GetPageFooter();
    return html;
}

// ====== API CONFIGURAZIONE ======

std::string GetConfigJson() {
    std::lock_guard<std::mutex> reloadLock(reloadMutex);
    std::lock_guard<std::mutex> lock(configMutex);
    std::ostringstream json;
    json << "{\n";
    json << "  \"configFile\": \"" << EscapeJsonString(configFile) << "\",\n";
    json << "  \"defaultMonitoredFolder\": \"" << EscapeJsonString(defaultMonitoredFolder) << "\",\n";
    json << "  \"logFile\": \"" << EscapeJsonString(logFile) << "\",\n";
    json << "  \"detailedLogFile\": \"" << EscapeJsonString(detailedLogFile) << "\",\n";
    json << "  \"processedFilesDb\": \"" << EscapeJsonString(processedFilesDb) << "\",\n";
    json << "  \"detailedLogging\": " << (detailedLogging ? "true" : "false") << ",\n";
    json << "  \"webServerPort\": " << webServerPort << ",\n";
    json << "  \"webServerEnabled\": " << (webServerEnabled ? "true" : "false") << ",\n";
    json << "  \"schedulerEnabled\": " << (schedulerEnabled ? "true" : "false") << ",\n";
    json << "  \"schedulerFolder\": \"" << EscapeJsonString(schedulerFolder) << "\",\n";
    json << "  \"reloadPending\": " << (configReloadRequested ? "true" : "false") << ",\n";
    json << "  \"lastReload\": \"" << EscapeJsonString(lastConfigReloadTime) << "\",\n";
    json << "  \"lastError\": \"" << EscapeJsonString(lastConfigReloadError) << "\",\n";
    json << "  \"patterns\": [\n";
    bool first = true;
    for (const auto& p : patternCommandPairs) {
        if (!first) json << ",\n";
        json << "    {\"name\": \"" << EscapeJsonString(p.patternName)
             << "\", \"folder\": \"" << EscapeJsonString(p.folderPath)
             << "\", \"regex\": \"" << EscapeJsonString(p.patternRegex)
             << "\", \"command\": \"" << EscapeJsonString(p.command) << "\"}";
        first = false;
    }
    json << "\n  ]\n}";
    return json.str();
}

static std::string HtmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

static std::string TrimCopy(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Valida e scrive la nuova configurazione su config.ini. In caso di errore restituisce false e
// imposta errorMessage; non tocca le variabili globali: sara' il ricaricamento ad applicarle.
bool SaveConfigurationFromJson(const std::string& body, std::string& errorMessage, bool& webRestart, int& newPort, bool& newWebEnabled) {
    std::string newDefaultFolder = TrimCopy(ExtractJsonValue(body, "defaultMonitoredFolder"));
    std::string newSchedFolder = TrimCopy(ExtractJsonValue(body, "schedulerFolder"));
    std::string newLogFile = TrimCopy(ExtractJsonValue(body, "logFile"));
    std::string newDetailedLog = TrimCopy(ExtractJsonValue(body, "detailedLogFile"));
    std::string newProcessedDb = TrimCopy(ExtractJsonValue(body, "processedFilesDb"));
    std::string detailedStr = ExtractJsonValue(body, "detailedLogging");
    std::string schedStr = ExtractJsonValue(body, "schedulerEnabled");
    std::string webStr = ExtractJsonValue(body, "webServerEnabled");
    std::string portStr = ExtractJsonValue(body, "webServerPort");
    std::string patternsStr = ExtractJsonValue(body, "patterns");

    if (newDefaultFolder.empty()) { errorMessage = "La cartella monitorata predefinita e' obbligatoria"; return false; }
    if (newSchedFolder.empty()) newSchedFolder = DEFAULT_SCHEDULER_FOLDER;
    if (newLogFile.empty()) newLogFile = DEFAULT_LOG_FILE;
    if (newDetailedLog.empty()) newDetailedLog = DEFAULT_DETAILED_LOG_FILE;
    if (newProcessedDb.empty()) newProcessedDb = DEFAULT_PROCESSED_FILES_DB;

    int port = 0;
    try { port = std::stoi(portStr); } catch (...) { port = 0; }
    if (port < 1 || port > 65535) { errorMessage = "Porta web server non valida (1-65535)"; return false; }

    bool newDetailed = (detailedStr == "true" || detailedStr == "1");
    bool newSched = (schedStr == "true" || schedStr == "1");
    bool newWeb = (webStr == "true" || webStr == "1");

    // Pattern: una riga per pattern, campi separati da tabulazione: nome, cartella, regex, comando
    struct NewPattern { std::string name, folder, regex, command; };
    std::vector<NewPattern> newPatterns;
    std::set<std::string> usedNames;
    std::istringstream pss(patternsStr);
    std::string line;
    int lineNo = 0;
    while (std::getline(pss, line)) {
        if (TrimCopy(line).empty()) continue;
        lineNo++;
        std::vector<std::string> f;
        std::string tmp = line;
        size_t pos;
        while ((pos = tmp.find('\t')) != std::string::npos) { f.push_back(tmp.substr(0, pos)); tmp.erase(0, pos + 1); }
        f.push_back(tmp);
        while (f.size() < 4) f.insert(f.begin(), "");
        NewPattern np;
        np.name = TrimCopy(f[0]);
        np.folder = TrimCopy(f[1]);
        np.regex = TrimCopy(f[2]);
        np.command = TrimCopy(f[3]);

        std::string safeName;
        for (char c : np.name) if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') safeName += c;
        if (safeName.empty()) safeName = "Pattern" + std::to_string(lineNo);
        int suffix = 1;
        std::string candidate = safeName;
        while (usedNames.count(candidate)) candidate = safeName + "_" + std::to_string(++suffix);
        np.name = candidate;
        usedNames.insert(candidate);

        if (np.regex.empty()) { errorMessage = "Il pattern " + np.name + " non ha una regex"; return false; }
        if (np.command.empty()) { errorMessage = "Il pattern " + np.name + " non ha un comando"; return false; }
        if (np.folder.empty() && np.regex.find('|') != std::string::npos) {
            errorMessage = "Il pattern " + np.name + " usa '|' nella regex: indica esplicitamente la cartella";
            return false;
        }
        try {
            std::regex test(np.regex, std::regex_constants::icase);
        } catch (const std::regex_error& e) {
            errorMessage = "Regex non valida nel pattern " + np.name + ": " + e.what();
            return false;
        }
        newPatterns.push_back(np);
    }

    std::lock_guard<std::mutex> lock(configMutex);

    std::string configDir = configFile.substr(0, configFile.find_last_of("\\/"));
    CreateDirectoryRecursive(configDir);

    // Scrittura atomica: file temporaneo poi sostituzione
    std::string tmpFile = configFile + ".tmp";
    {
        std::ofstream out(tmpFile.c_str(), std::ios::trunc);
        if (!out.is_open()) { errorMessage = "Impossibile scrivere il file di configurazione: " + configFile; return false; }
        out << "# PatternTriggerCommand Configuration - Multi-Folder Support + Web Dashboard\n";
        out << "# Autore: Umberto Meglio - Supporto: Claude di Anthropic\n";
        out << "# Aggiornato dalla dashboard web il " << GetTimestamp() << "\n\n";
        out << "[Settings]\n";
        out << "DefaultMonitoredFolder=" << newDefaultFolder << "\n";
        out << "LogFile=" << newLogFile << "\n";
        out << "DetailedLogFile=" << newDetailedLog << "\n";
        out << "ProcessedFilesDB=" << newProcessedDb << "\n";
        out << "DetailedLogging=" << (newDetailed ? "true" : "false") << "\n";
        out << "WebServerPort=" << port << "\n";
        out << "WebServerEnabled=" << (newWeb ? "true" : "false") << "\n";
        out << "SchedulerEnabled=" << (newSched ? "true" : "false") << "\n";
        out << "SchedulerFolder=" << newSchedFolder << "\n\n";
        out << "[Patterns]\n";
        out << "# Formato esteso: Nome=Cartella|Regex|Comando\n";
        out << "# Formato legacy: Nome=Regex|Comando (usa la cartella predefinita)\n";
        for (const auto& p : newPatterns) {
            out << p.name << "=";
            if (!p.folder.empty()) out << p.folder << "|";
            out << p.regex << "|" << p.command << "\n";
        }
        if (!out.good()) { errorMessage = "Errore durante la scrittura del file di configurazione"; return false; }
    }
    if (!MoveFileExA(tmpFile.c_str(), configFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmpFile.c_str());
        errorMessage = "Impossibile sostituire il file di configurazione (errore " + std::to_string(GetLastError()) + ")";
        return false;
    }

    webRestart = (newWeb != webServerEnabled) || (newWeb && port != webServerPort);
    newPort = port;
    newWebEnabled = newWeb;
    WriteToLog("Configurazione salvata dalla dashboard web (" + std::to_string(newPatterns.size()) + " pattern)");
    return true;
}

// ====== FONT INCORPORATI E STATISTICHE ======

static std::string DecodeBase64(const char* input) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t len = std::strlen(input);
    out.reserve(len * 3 / 4);
    int val = 0, bits = -8;
    for (size_t i = 0; i < len; ++i) {
        char c = input[i];
        if (c == '=') break;
        size_t pos = chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// Restituisce il contenuto binario di un font miraFONT incorporato (decodificato una sola volta)
static const std::string* GetEmbeddedFont(const std::string& name) {
    static std::map<std::string, std::string> cache;
    static std::mutex cacheMutex;
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = cache.find(name);
    if (it != cache.end()) return &it->second;

    const char* b64 = NULL;
    if (name == "miraFONT-Regular-Spline.woff") b64 = MIRAFONT_SPLINE_REGULAR_B64;
    else if (name == "miraFONT-Bold-Spline.woff") b64 = MIRAFONT_SPLINE_BOLD_B64;
    else if (name == "miraFONT-Regular-Linear.woff") b64 = MIRAFONT_LINEAR_REGULAR_B64;
    else if (name == "miraFONT-Regular-Dots.woff") b64 = MIRAFONT_DOTS_REGULAR_B64;
    if (!b64) return NULL;

    cache[name] = DecodeBase64(b64);
    return &cache[name];
}

void RecordStatsSample() {
    std::lock_guard<std::mutex> lock(statsMutex);
    StatsSample s;
    s.timestamp = std::time(NULL);
    s.filesProcessed = systemMetrics.totalFilesProcessed.load();
    s.commandsExecuted = systemMetrics.commandsExecuted.load();
    s.errors = systemMetrics.errorsCount.load();
    s.memoryMB = systemMetrics.memoryUsageMB.load();
    statsSamples.push_back(s);
    if (statsSamples.size() > MAX_STATS_SAMPLES) {
        statsSamples.erase(statsSamples.begin(), statsSamples.begin() + (statsSamples.size() - MAX_STATS_SAMPLES));
    }
}

// Azzera i contatori giornalieri quando cambia il giorno
void CheckDailyStatsRollover() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    int today = st.wYear * 1000 + st.wDay + st.wMonth * 40;
    if (statsCurrentDay != today) {
        if (statsCurrentDay != -1) {
            systemMetrics.filesProcessedToday = 0;
            for (int h = 0; h < 24; ++h) { hourlyFiles[h] = 0; hourlyCommands[h] = 0; }
            WriteToLog("Nuovo giorno: azzerati i contatori giornalieri", true);
        }
        statsCurrentDay = today;
    }
}

void RecordHourlyFile() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    hourlyFiles[st.wHour % 24]++;
}

void RecordHourlyCommand() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    hourlyCommands[st.wHour % 24]++;
}

std::string GetStatsJson() {
    std::lock_guard<std::mutex> reloadLock(reloadMutex);
    std::ostringstream json;
    auto now = std::chrono::steady_clock::now();
    auto uptimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - systemMetrics.serviceStartTime).count();
    size_t total = systemMetrics.totalFilesProcessed.load();
    size_t cmds = systemMetrics.commandsExecuted.load();
    size_t errs = systemMetrics.errorsCount.load();

    json << "{\n";
    json << "  \"now\": " << std::time(NULL) << ",\n";
    json << "  \"uptimeSeconds\": " << uptimeSeconds << ",\n";
    json << "  \"totalFilesProcessed\": " << total << ",\n";
    json << "  \"filesProcessedToday\": " << systemMetrics.filesProcessedToday.load() << ",\n";
    json << "  \"commandsExecuted\": " << cmds << ",\n";
    json << "  \"errorsCount\": " << errs << ",\n";
    json << "  \"averageProcessingTime\": " << systemMetrics.averageProcessingTime.load() << ",\n";
    json << "  \"filesPerHour\": " << (uptimeSeconds > 0 ? (static_cast<double>(total) * 3600.0 / static_cast<double>(uptimeSeconds)) : 0.0) << ",\n";
    json << "  \"successRate\": " << (cmds + errs > 0 ? (100.0 * static_cast<double>(cmds) / static_cast<double>(cmds + errs)) : 100.0) << ",\n";

    json << "  \"hourlyFiles\": [";
    for (int h = 0; h < 24; ++h) json << (h ? "," : "") << hourlyFiles[h].load();
    json << "],\n  \"hourlyCommands\": [";
    for (int h = 0; h < 24; ++h) json << (h ? "," : "") << hourlyCommands[h].load();
    json << "],\n";

    json << "  \"samples\": [";
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        bool first = true;
        for (const auto& s : statsSamples) {
            if (!first) json << ",";
            json << "{\"t\":" << s.timestamp << ",\"f\":" << s.filesProcessed << ",\"c\":" << s.commandsExecuted
                 << ",\"e\":" << s.errors << ",\"m\":" << s.memoryMB << "}";
            first = false;
        }
    }
    json << "],\n";

    json << "  \"patterns\": [";
    {
        std::lock_guard<std::mutex> lock(patternStatsMutex);
        bool first = true;
        for (const auto& p : patternCommandPairs) {
            if (!first) json << ",";
            json << "{\"name\":\"" << EscapeJsonString(p.patternName) << "\",\"folder\":\"" << EscapeJsonString(p.folderPath)
                 << "\",\"match\":" << patternMatchCounts[p.patternName] << ",\"exec\":" << patternExecutionCounts[p.patternName] << "}";
            first = false;
        }
    }
    json << "],\n";

    json << "  \"folders\": [";
    {
        bool first = true;
        for (const auto& m : folderMonitors) {
            if (!first) json << ",";
            json << "{\"path\":\"" << EscapeJsonString(m.second->folderPath) << "\",\"detected\":" << m.second->filesDetected.load()
                 << ",\"processed\":" << m.second->filesProcessed.load() << "}";
            first = false;
        }
    }
    json << "],\n";

    {
        std::lock_guard<std::mutex> lock(schedulerMutex);
        size_t ok = 0, fail = 0;
        for (const auto& e : schedulerHistory) { if (e.success) ok++; else fail++; }
        size_t enabledTasks = 0;
        for (const auto& t : schedulerTasks) if (t.enabled) enabledTasks++;
        json << "  \"scheduler\": {\"ok\": " << ok << ", \"fail\": " << fail << ", \"tasks\": " << schedulerTasks.size()
             << ", \"enabledTasks\": " << enabledTasks << ", \"enabled\": " << (schedulerEnabled ? "true" : "false") << ", \"topTasks\": [";
        std::vector<const SchedulerTask*> sorted;
        for (const auto& t : schedulerTasks) sorted.push_back(&t);
        std::sort(sorted.begin(), sorted.end(), [](const SchedulerTask* a, const SchedulerTask* b) { return a->executionCount > b->executionCount; });
        for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
            if (i) json << ",";
            json << "{\"name\":\"" << EscapeJsonString(sorted[i]->name) << "\",\"exec\":" << sorted[i]->executionCount << "}";
        }
        json << "]}\n";
    }
    json << "}";
    return json.str();
}

// ====== SERVER HTTP ======

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

static HttpRequest ParseHttpRequest(const std::string& raw) {
    HttpRequest req;
    size_t lineEnd = raw.find("\r\n");
    std::string requestLine = raw.substr(0, lineEnd == std::string::npos ? raw.size() : lineEnd);
    std::istringstream rl(requestLine);
    std::string target;
    rl >> req.method >> target;
    size_t q = target.find('?');
    req.path = (q == std::string::npos) ? target : target.substr(0, q);
    if (req.path.empty()) req.path = "/";
    req.body = GetHttpRequestBody(raw);
    return req;
}

static std::string BuildHttpResponse(const std::string& status, const std::string& contentType, const std::string& body) {
    std::string response = "HTTP/1.1 " + status + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(body.length()) + "\r\n";
    response += "Cache-Control: no-cache, no-store\r\n";
    response += "X-Content-Type-Options: nosniff\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;
    return response;
}

static std::string HtmlResponse(const std::string& html) { return BuildHttpResponse("200 OK", "text/html; charset=utf-8", html); }
static std::string JsonResponse(const std::string& json) { return BuildHttpResponse("200 OK", "application/json; charset=utf-8", json); }

std::string HandleHttpRequest(const std::string& request) {
    HttpRequest req = ParseHttpRequest(request);
    const std::string& path = req.path;
    bool isGet = (req.method == "GET" || req.method == "HEAD");
    bool isPost = (req.method == "POST");

    if (isGet && (path == "/" || path == "/dashboard" || path == "/index.html")) {
        return HtmlResponse(GetDashboardHtml());
    }
    if (isGet && path == "/scheduler") {
        return HtmlResponse(GetSchedulerPageHtml());
    }
    if (isGet && (path == "/settings" || path == "/config")) {
        return HtmlResponse(GetSettingsPageHtml());
    }
    if (isGet && path == "/favicon.ico") {
        return "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    }
    if (isGet && path == "/api/stats") {
        return JsonResponse(GetStatsJson());
    }
    if (isGet && path.compare(0, 7, "/fonts/") == 0) {
        const std::string* font = GetEmbeddedFont(path.substr(7));
        if (!font) return BuildHttpResponse("404 Not Found", "text/plain", "Font non trovato");
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: font/woff\r\nContent-Length: " + std::to_string(font->size()) +
                               "\r\nCache-Control: public, max-age=31536000, immutable\r\nConnection: close\r\n\r\n";
        response += *font;
        return response;
    }
    if (isGet && path == "/api/metrics") {
        return JsonResponse(GetSystemMetricsJson());
    }
    if (isGet && path == "/api/scheduler/scripts") {
        return JsonResponse(GetSchedulerScriptsJson());
    }
    if (isGet && path == "/api/scheduler") {
        return JsonResponse(GetSchedulerJson());
    }
    if (isGet && path == "/api/config") {
        return JsonResponse(GetConfigJson());
    }
    if (isPost && path == "/api/config/save") {
        std::string error;
        bool webRestart = false, newWebEnabled = true;
        int newPort = webServerPort;
        std::string resultJson;
        if (SaveConfigurationFromJson(req.body, error, webRestart, newPort, newWebEnabled)) {
            configReloadRequested = true;
            resultJson = std::string("{\"success\": true, \"webRestart\": ") + (webRestart ? "true" : "false") +
                         ", \"webEnabled\": " + (newWebEnabled ? "true" : "false") +
                         ", \"newPort\": " + std::to_string(newPort) + "}";
        } else {
            resultJson = "{\"success\": false, \"error\": \"" + EscapeJsonString(error) + "\"}";
        }
        return JsonResponse(resultJson);
    }
    if (isPost && path == "/api/config/reload") {
        WriteToLog("Ricaricamento configurazione richiesto dalla dashboard web");
        configReloadRequested = true;
        return JsonResponse("{\"success\": true}");
    }
    if (isPost && path == "/api/scheduler/save") {
        const std::string& body = req.body;
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

            std::istringstream dss(daysStr);
            std::string day;
            while (std::getline(dss, day, ',')) {
                day = TrimCopy(day);
                int d = DayNameToNumber(day);
                if (d >= 0) task.days.insert(d);
            }

            std::istringstream hss(hoursStr);
            std::string hour;
            while (std::getline(hss, hour, ',')) {
                hour = TrimCopy(hour);
                try { int h = std::stoi(hour); if (h >= 0 && h <= 23) task.hours.insert(h); } catch (...) {}
            }

            std::istringstream mss(minutesStr);
            std::string minute;
            while (std::getline(mss, minute, ',')) {
                minute = TrimCopy(minute);
                try { int m = std::stoi(minute); if (m >= 0 && m <= 59) task.minutes.insert(m); } catch (...) {}
            }

            if (task.intervalSeconds <= 0 && (task.days.empty() || task.hours.empty() || task.minutes.empty())) {
                resultJson = "{\"success\": false, \"error\": \"Indica giorni, ore e minuti validi oppure un intervallo\"}";
            } else if (SaveSchedulerTask(task)) {
                LoadSchedulerTasks();
                resultJson = "{\"success\": true}";
            } else {
                resultJson = "{\"success\": false, \"error\": \"Errore salvataggio su disco\"}";
            }
        }
        return JsonResponse(resultJson);
    }
    if (isPost && path == "/api/scheduler/delete") {
        std::string name = ExtractJsonValue(req.body, "name");
        std::string resultJson;
        if (!name.empty() && DeleteSchedulerTask(name)) {
            resultJson = "{\"success\": true}";
        } else {
            resultJson = "{\"success\": false, \"error\": \"Task non trovato\"}";
        }
        return JsonResponse(resultJson);
    }
    if (isPost && path == "/api/scheduler/toggle") {
        std::string name = ExtractJsonValue(req.body, "name");
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
        return JsonResponse(resultJson);
    }

    if (path.compare(0, 5, "/api/") == 0) {
        return BuildHttpResponse("404 Not Found", "application/json; charset=utf-8", "{\"success\": false, \"error\": \"Endpoint non trovato\"}");
    }

    std::string notFound = GetPageHeader("Pagina non trovata", "");
    notFound += "<div class=\"card\" style=\"text-align:center;padding:60px 20px\"><h2 style=\"font-size:3em;color:var(--primary)\">404</h2><p style=\"color:var(--text2);margin:10px 0 20px\">La pagina <span class=\"mono\">" + HtmlEscape(path) + "</span> non esiste.</p><a class=\"btn btn-primary\" href=\"/\">Torna alla dashboard</a></div>";
    notFound += GetPageFooter();
    return BuildHttpResponse("404 Not Found", "text/html; charset=utf-8", notFound);
}

static bool SendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(5); continue; }
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Legge una richiesta HTTP completa (intestazioni + body indicato da Content-Length)
static bool ReadHttpRequest(SOCKET s, std::string& request) {
    const size_t MAX_REQUEST = 2 * 1024 * 1024;
    char buffer[8192];
    size_t headerEnd = std::string::npos;
    size_t contentLength = 0;

    while (true) {
        int n = recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0) return !request.empty() && headerEnd != std::string::npos && request.size() >= headerEnd + 4 + contentLength;
        request.append(buffer, static_cast<size_t>(n));
        if (request.size() > MAX_REQUEST) return false;

        if (headerEnd == std::string::npos) {
            headerEnd = request.find("\r\n\r\n");
            if (headerEnd == std::string::npos) continue;

            std::string headers = request.substr(0, headerEnd);
            std::string lower = headers;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            size_t clPos = lower.find("content-length:");
            if (clPos != std::string::npos) {
                try { contentLength = static_cast<size_t>(std::stoul(TrimCopy(headers.substr(clPos + 15, headers.find("\r\n", clPos) - clPos - 15)))); }
                catch (...) { contentLength = 0; }
            }
            if (contentLength > MAX_REQUEST) return false;
        }
        if (request.size() >= headerEnd + 4 + contentLength) return true;
    }
}

void HandleClientConnection(SOCKET clientSocket) {
    webServerActiveConnections++;

    u_long blocking = 0;
    ioctlsocket(clientSocket, FIONBIO, &blocking);
    DWORD timeout = 5000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    BOOL noDelay = TRUE;
    setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&noDelay, sizeof(noDelay));

    std::string request;
    std::string response;
    if (ReadHttpRequest(clientSocket, request)) {
        try {
            response = HandleHttpRequest(request);
        } catch (const std::exception& e) {
            WriteToLog("ERRORE gestione richiesta HTTP: " + std::string(e.what()));
            systemMetrics.errorsCount++;
            response = BuildHttpResponse("500 Internal Server Error", "application/json; charset=utf-8",
                                         "{\"success\": false, \"error\": \"" + EscapeJsonString(e.what()) + "\"}");
        } catch (...) {
            response = BuildHttpResponse("500 Internal Server Error", "text/plain", "Errore interno");
        }
    } else if (!request.empty()) {
        response = BuildHttpResponse("400 Bad Request", "text/plain", "Richiesta non valida o incompleta");
    }

    if (!response.empty()) {
        SendAll(clientSocket, response);
    }
    shutdown(clientSocket, SD_SEND);
    // Attende brevemente che il client chiuda, per evitare RST con dati non ancora letti
    char drain[512];
    timeout = 300;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    recv(clientSocket, drain, sizeof(drain), 0);
    closesocket(clientSocket);

    webServerActiveConnections--;
}

void WebServerWorker() {
    WriteToLog("Avvio web server sulla porta " + std::to_string(webServerPort));
    webServerShouldStop = false;

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        WriteToLog("ERRORE: Creazione socket fallita");
        systemMetrics.errorsCount++;
        return;
    }

    BOOL exclusive = TRUE;
    setsockopt(serverSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&exclusive, sizeof(exclusive));

    sockaddr_in serverAddr;
    ZeroMemory(&serverAddr, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<u_short>(webServerPort));

    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        WriteToLog("ERRORE: Bind socket fallito sulla porta " + std::to_string(webServerPort) +
                   " (errore " + std::to_string(WSAGetLastError()) + "): porta occupata?");
        systemMetrics.errorsCount++;
        closesocket(serverSocket);
        return;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        WriteToLog("ERRORE: Listen socket fallito");
        systemMetrics.errorsCount++;
        closesocket(serverSocket);
        return;
    }

    webServerRunning = true;
    WriteToLog("Web server avviato su http://localhost:" + std::to_string(webServerPort));

    while (!globalShutdown && !webServerShouldStop) {
        // Attesa connessioni con select: reattivo allo stop senza polling aggressivo
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverSocket, &readSet);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000;

        int ready = select(0, &readSet, NULL, NULL, &tv);
        if (ready == SOCKET_ERROR) {
            WriteToLog("ERRORE: select fallito: " + std::to_string(WSAGetLastError()));
            systemMetrics.errorsCount++;
            break;
        }
        if (ready == 0) continue;

        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAECONNRESET || error == WSAEINTR) continue;
            if (!globalShutdown && !webServerShouldStop) {
                WriteToLog("ERRORE: Accept fallito: " + std::to_string(error));
                systemMetrics.errorsCount++;
            }
            break;
        }

        if (globalShutdown || webServerShouldStop) {
            closesocket(clientSocket);
            break;
        }

        // Ogni connessione viene servita in un thread dedicato: le pagine grandi e le richieste
        // concorrenti del browser non si bloccano piu' a vicenda
        try {
            std::thread(HandleClientConnection, clientSocket).detach();
        } catch (...) {
            HandleClientConnection(clientSocket);
        }
    }

    closesocket(serverSocket);

    // Attende le connessioni ancora in corso (max 2 secondi)
    for (int i = 0; i < 20 && webServerActiveConnections > 0; ++i) Sleep(100);

    webServerRunning = false;
    WriteToLog("Web server terminato");
}

void StartWebServer() {
    if (!webServerEnabled) return;
    if (webServerThread.joinable()) return;
    webServerShouldStop = false;
    webServerThread = std::thread(WebServerWorker);
}

void StopWebServer() {
    if (!webServerThread.joinable()) return;
    webServerShouldStop = true;
    WriteToLog("Arresto web server...");

    auto start = GetTickCount();
    while (GetTickCount() - start < 3000 && webServerRunning) Sleep(50);

    if (!webServerRunning) {
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
    webServerShouldStop = false;
}

// Ricarica la configurazione da disco e la applica a caldo: ferma i monitor, rilegge config.ini,
// riavvia i monitor e (se necessario) il web server sulla nuova porta.
void ApplyConfigurationReload() {
    std::lock_guard<std::mutex> reloadLock(reloadMutex);
    WriteToLog("=== Ricaricamento configurazione a caldo ===");

    int oldPort = webServerPort;
    bool oldWebEnabled = webServerEnabled;
    std::string reloadError;

    SaveProcessedFiles();
    StopAllFolderMonitors();

    if (!LoadConfiguration()) {
        reloadError = "config.ini non valido o illeggibile: verificare il file";
        WriteToLog("ERRORE: Ricaricamento configurazione fallito");
    }

    LoadProcessedFiles();
    LoadSchedulerTasks();
    StartAllFolderMonitors();

    {
        std::lock_guard<std::mutex> lock(configMutex);
        lastConfigReloadTime = GetTimestamp();
        lastConfigReloadError = reloadError;
    }

    if (webServerEnabled != oldWebEnabled || webServerPort != oldPort) {
        WriteToLog("Riavvio web server: porta " + std::to_string(oldPort) + " -> " + std::to_string(webServerPort) +
                   (webServerEnabled ? "" : " (disabilitato)"));
        StopWebServer();
        StartWebServer();
    }

    WriteToLog("Configurazione applicata: " + std::to_string(patternCommandPairs.size()) + " pattern, " +
               std::to_string(folderMonitors.size()) + " cartelle, " + std::to_string(schedulerTasks.size()) + " task");
}


DWORD WINAPI ServiceWorkerThread(LPVOID /*lpParam*/) {
    WriteToLog("=== Servizio PatternTriggerCommand Multi-Folder v3.1 avviato ===");
    
    if (!LoadConfiguration()) {
        WriteToLog("ERRORE FATALE: Impossibile caricare configurazione");
        return 1;
    }

    WSADATA wsaData;
    bool wsaInitialized = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    if (!wsaInitialized) {
        WriteToLog("ERRORE: WSAStartup fallito, web server non disponibile");
        systemMetrics.errorsCount++;
    }
    
    std::string scriptsDir = "C:\\Scripts";
    CreateDirectoryRecursive(scriptsDir);
    
    LoadProcessedFiles();
    
    recentlyIgnoredFiles.clear();
    
    // Avvia thread aggiornamento metriche
    std::thread metricsThread(MetricsUpdateWorker);
    
    // Avvia web server se abilitato
    if (wsaInitialized) {
        StartWebServer();
    }

    // Carica e avvia schedulatore (il thread resta in attesa se disabilitato, cosi' puo' essere
    // riattivato a caldo dalla dashboard)
    LoadSchedulerTasks();
    schedulerThread = std::thread(SchedulerWorker);
    if (schedulerEnabled) {
        WriteToLog("Schedulatore avviato con " + std::to_string(schedulerTasks.size()) + " task");
    } else {
        WriteToLog("Schedulatore disabilitato da configurazione");
    }

    StartAllFolderMonitors();

    WriteToLog("Servizio in esecuzione, attesa terminazione...");
    if (webServerEnabled) {
        WriteToLog("Dashboard disponibile su: http://localhost:" + std::to_string(webServerPort));
    }
    
    DWORD lastCleanup = GetTickCount();
    int loopTick = 0;
    
    while (!globalShutdown) {
        if (WaitForSingleObject(stopEvent, 1000) == WAIT_OBJECT_0) {
            break;
        }

        // Ricaricamento configurazione richiesto dalla dashboard web
        if (configReloadRequested) {
            ApplyConfigurationReload();
            configReloadRequested = false;
        }

        if (++loopTick % 5 != 0) continue;
        
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
    
    // 1. Ferma web server per primo (TIMEOUT: 3 secondi)
    StopWebServer();
    
    // 2. Ferma schedulatore - globalShutdown gia' impostato, lo sleep frazionato lo sblocca in <100ms
    if (schedulerThread.joinable()) {
        WriteToLog("Arresto thread schedulatore...");
        try {
            schedulerThread.join();
            WriteToLog("Thread schedulatore arrestato");
        } catch (...) {
            WriteToLog("ERRORE join schedulatore - detach forzato");
            schedulerThread.detach();
        }
    }

    // 3. Ferma monitor cartelle
    WriteToLog("Arresto monitor cartelle...");
    StopAllFolderMonitors();

    // 4. Ferma thread metriche - globalShutdown gia' impostato, lo sleep frazionato lo sblocca in <100ms
    if (metricsThread.joinable()) {
        WriteToLog("Arresto thread metriche...");
        try {
            metricsThread.join();
            WriteToLog("Thread metriche arrestato");
        } catch (...) {
            WriteToLog("ERRORE join metriche - detach forzato");
            metricsThread.detach();
        }
    }
    
    SaveProcessedFiles();

    if (wsaInitialized) WSACleanup();
    
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

        // Chiudi handle directory per sbloccare ReadDirectoryChangesW
        for (auto& monitorPair : folderMonitors) {
            if (monitorPair.second->directoryHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(monitorPair.second->directoryHandle);
                monitorPair.second->directoryHandle = INVALID_HANDLE_VALUE;
            }
        }

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
    
    WriteToLog("=== PatternTriggerCommand Multi-Folder v3.1 + Dashboard ===");
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
                std::cout << "Impostazioni: http://localhost:" << webServerPort << "/settings" << std::endl;
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
            
            std::cout << "=== Status PatternTriggerCommand v3.1 ===" << std::endl;
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
