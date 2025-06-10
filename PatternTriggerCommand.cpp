#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <regex>
#include <direct.h>  // Per _mkdir
#include <set>       // Per tenere traccia dei file processati
#include <ctime>     // Per timestamp
#include <iomanip>   // Per formattare l'output nel log
#include <algorithm> // Per std::transform e case insensitive check
#include <map>       // Per la mappatura pattern-comando

// Autore: Umberto Meglio
// Supporto alla creazione: Claude di Anthropic

// Definizioni necessarie per compatibilità MinGW
#ifndef ERROR_OPERATION_ABORTED
#define ERROR_OPERATION_ABORTED 995L
#endif

#ifndef SERVICE_CONFIG_DESCRIPTION
#define SERVICE_CONFIG_DESCRIPTION 1
#endif

// Questa funzione viene caricata dinamicamente per migliorare la compatibilità
// con diverse versioni di MinGW
typedef BOOL (WINAPI *CHANGESERVICECONFIG2PROC)(
    SC_HANDLE hService,
    DWORD dwInfoLevel,
    LPVOID lpInfo
);

BOOL MyChangeServiceConfig2(
    SC_HANDLE hService,
    DWORD dwInfoLevel,
    LPVOID lpInfo
) {
    HMODULE hModule = LoadLibrary("advapi32.dll");
    if (hModule == NULL) {
        return FALSE;
    }
    
    CHANGESERVICECONFIG2PROC changeServiceConfig2Proc = 
        (CHANGESERVICECONFIG2PROC)GetProcAddress(hModule, "ChangeServiceConfig2A");
    
    BOOL result = FALSE;
    if (changeServiceConfig2Proc != NULL) {
        result = changeServiceConfig2Proc(hService, dwInfoLevel, lpInfo);
    }
    
    FreeLibrary(hModule);
    return result;
}

// Strutture necessarie per compatibilità MinGW
struct SERVICE_DESCRIPTION_STRUCT {
    LPSTR lpDescription;
};

// Nome del servizio
#define SERVICE_NAME "PatternTriggerCommand"
#define SERVICE_DISPLAY_NAME "Pattern Trigger Command Service"
#define SERVICE_DESCRIPTION "Monitora cartelle multiple per file che corrispondono a pattern configurati ed esegue comandi associati"

// Percorsi di default (possono essere sovrascritti dal file di configurazione)
#define DEFAULT_MONITORED_FOLDER "C:\\Monitored"
#define DEFAULT_CONFIG_FILE "C:\\PTC\\config.ini"
#define DEFAULT_LOG_FILE "C:\\PTC\\PatternTriggerCommand.log"
#define DEFAULT_DETAILED_LOG_FILE "C:\\PTC\\PatternTriggerCommand_detailed.log"
#define DEFAULT_PROCESSED_FILES_DB "C:\\PTC\\PatternTriggerCommand_processed.txt"

// Intervalli di tempo (in millisecondi)
#define FILE_CHECK_INTERVAL 5000      // 5 secondi tra una verifica e l'altra
#define MONITORING_RESTART_DELAY 3000 // 3 secondi dopo il batch prima di riavviare il monitoraggio
#define BATCH_TIMEOUT 60000           // 60 secondi di timeout per il batch
#define CACHE_CLEANUP_INTERVAL 600000 // 10 minuti per la pulizia della cache
#define SERVICE_SHUTDOWN_TIMEOUT 10000 // 10 secondi massimo per l'arresto del servizio
#define DIR_CHANGES_WAIT_TIMEOUT 1000 // 1 secondo massimo di attesa per le modifiche directory

// Stato del servizio
SERVICE_STATUS serviceStatus;
SERVICE_STATUS_HANDLE serviceStatusHandle;

// Handle per l'evento che controlla il servizio
HANDLE stopEvent = NULL;

// Set per tracciare i file già processati (include il percorso completo per evitare conflitti)
std::set<std::string> processedFiles;

// Set per tracciare i file ignorati di recente (per evitare log ripetitivi)
std::set<std::string> recentlyIgnoredFiles;

// Flag per l'esecuzione dettagliata del log
bool detailedLogging = true;

// Percorsi configurabili
std::string defaultMonitoredFolder = DEFAULT_MONITORED_FOLDER; // Cartella di default per retrocompatibilità
std::string configFile = DEFAULT_CONFIG_FILE;
std::string logFile = DEFAULT_LOG_FILE;
std::string detailedLogFile = DEFAULT_DETAILED_LOG_FILE;
std::string processedFilesDb = DEFAULT_PROCESSED_FILES_DB;

// Struttura migliorata per memorizzare i pattern, le cartelle e i comandi associati
struct PatternCommandPair {
    std::string folderPath;         // Cartella da monitorare per questo pattern
    std::string patternRegex;       // Pattern come espressione regolare
    std::string command;            // Comando da eseguire
    std::regex compiledRegex;       // Pattern compilato per efficienza
    std::string patternName;        // Nome del pattern per identificazione
    
    PatternCommandPair(const std::string& folder, const std::string& pattern, 
                      const std::string& cmd, const std::string& name = "") 
        : folderPath(folder), patternRegex(pattern), command(cmd), 
          compiledRegex(pattern), patternName(name) {}
};

// Vettore di configurazioni pattern-comando con supporto multi-cartella
std::vector<PatternCommandPair> patternCommandPairs;

// Mappa per tenere traccia delle cartelle uniche e i loro handle di monitoraggio
struct FolderMonitorInfo {
    HANDLE hDir;
    std::vector<int> patternIndices; // Indici dei pattern associati a questa cartella
    OVERLAPPED overlapped;
    BYTE buffer[4096];
    bool active;
};
std::map<std::string, FolderMonitorInfo> monitoredFolders;

// Funzione per ottenere un timestamp formattato
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

// Funzione per scrivere log
void WriteToLog(const std::string& message, bool detailed = false) {
    std::ofstream logFileStream(logFile.c_str(), std::ios::app);
    if (logFileStream.is_open()) {
        logFileStream << GetTimestamp() << " - " << message << std::endl;
        logFileStream.close();
    }
    
    // Se è richiesto il log dettagliato e il flag è attivo, scrivi anche nel file di log dettagliato
    if (detailed && detailedLogging) {
        std::ofstream detailedLogFileStream(detailedLogFile.c_str(), std::ios::app);
        if (detailedLogFileStream.is_open()) {
            detailedLogFileStream << GetTimestamp() << " - " << message << std::endl;
            detailedLogFileStream.close();
        }
    }
}

// Normalizza il percorso di una cartella
std::string NormalizeFolderPath(const std::string& path) {
    std::string normalized = path;
    // Sostituisci / con \
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    
    // Rimuovi trailing slash se presente
    if (!normalized.empty() && normalized.back() == '\\') {
        normalized.pop_back();
    }
    
    // Converti tutto in maiuscolo per confronti case-insensitive
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::toupper);
    
    return normalized;
}

// Funzione per verificare se dobbiamo loggare un file ignorato
bool ShouldLogIgnoredFile(const std::string& filename) {
    // Se il file non è nell'elenco dei recenti, logghiamolo e aggiungiamolo
    if (recentlyIgnoredFiles.find(filename) == recentlyIgnoredFiles.end()) {
        recentlyIgnoredFiles.insert(filename);
        return true;
    }
    return false;
}

// Funzione per pulire l'elenco dei file ignorati recentemente
void CleanupRecentlyIgnoredFiles() {
    recentlyIgnoredFiles.clear();
    WriteToLog("Pulizia della cache dei file ignorati", true);
}

// Funzione per caricare i file già processati
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
        WriteToLog("Caricati " + std::to_string(processedFiles.size()) + " file già processati dal database");
    } else {
        WriteToLog("Database dei file processati non trovato, verrà creato al primo utilizzo");
    }
    
    // Log dei file già processati per debug
    if (detailedLogging && !processedFiles.empty()) {
        WriteToLog("Elenco dei file già processati:", true);
        for (const auto& filename : processedFiles) {
            WriteToLog("  - " + filename, true);
        }
    }
}

// Salva il database dei file processati
void SaveProcessedFiles() {
    std::ofstream file(processedFilesDb.c_str());
    
    if (file.is_open()) {
        for (const auto& filename : processedFiles) {
            file << filename << std::endl;
        }
        file.close();
        WriteToLog("Salvati " + std::to_string(processedFiles.size()) + " file processati nel database");
    } else {
        WriteToLog("ERRORE: Impossibile salvare il database dei file processati");
    }
    
    // Log dettagliato
    WriteToLog("Database dei file processati aggiornato", true);
}

// Funzione per aggiungere un file al set dei processati (ora include il percorso completo)
void MarkFileAsProcessed(const std::string& fullFilePath) {
    // Log di debug prima dell'inserimento
    WriteToLog("Marcando file come processato: " + fullFilePath, true);
    
    // Verifica se il file è già stato processato (per sicurezza)
    if (processedFiles.find(fullFilePath) != processedFiles.end()) {
        WriteToLog("AVVISO: Il file " + fullFilePath + " era già presente nel database", true);
    } else {
        WriteToLog("Nuovo file aggiunto al database: " + fullFilePath, true);
    }
    
    // Aggiungi al set e salva
    processedFiles.insert(fullFilePath);
    
    // Salva immediatamente il database per evitare perdite in caso di crash
    SaveProcessedFiles();
}

// Funzione per verificare se un file è stato già processato (ora usa il percorso completo)
bool IsFileAlreadyProcessed(const std::string& fullFilePath) {
    return processedFiles.find(fullFilePath) != processedFiles.end();
}

// Verifica l'esistenza di una directory e la crea se non esiste
bool EnsureDirectoryExists(const std::string& path) {
    // Controlla se la directory esiste
    DWORD attrs = GetFileAttributes(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;  // La directory esiste
    }
    
    // Crea la directory
    if (_mkdir(path.c_str()) == 0) {
        WriteToLog("Directory creata: " + path);
        return true;
    } else {
        WriteToLog("Errore nella creazione della directory: " + path + " - " + std::to_string(GetLastError()));
        return false;
    }
}

// Crea path ricorsivo (simile a mkdir -p)
bool CreateDirectoryRecursive(const std::string& path) {
    // Conversione a path nativo Windows
    std::string normalizedPath = path;
    // Sostituisci / con \
    std::replace(normalizedPath.begin(), normalizedPath.end(), '/', '\\');
    
    // Rimuovi trailing slash
    if (!normalizedPath.empty() && (normalizedPath.back() == '\\')) {
        normalizedPath.pop_back();
    }
    
    // Controlla se esiste già
    if (EnsureDirectoryExists(normalizedPath)) {
        return true;
    }
    
    // Cerca l'ultimo separatore
    size_t pos = normalizedPath.find_last_of('\\');
    if (pos == std::string::npos) {
        // Nessun separatore, non possiamo procedere
        return false;
    }
    
    // Crea il parent
    std::string parentPath = normalizedPath.substr(0, pos);
    if (!CreateDirectoryRecursive(parentPath)) {
        return false;
    }
    
    // Ora crea la directory finale
    return EnsureDirectoryExists(normalizedPath);
}

// Verifica l'esistenza di un file
bool FileExists(const std::string& filename) {
    DWORD attrs = GetFileAttributes(filename.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;  // Il file esiste
    }
    
    return false;
}

// Verifica se un file è in uso
bool IsFileInUse(const std::string& filePath) {
    HANDLE hFile = CreateFile(
        filePath.c_str(),
        GENERIC_READ,
        0,  // No share mode = accesso esclusivo
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    
    if (hFile != INVALID_HANDLE_VALUE) {
        // File non in uso
        CloseHandle(hFile);
        return false;
    }
    
    DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        // File in uso
        return true;
    }
    
    // Altro errore
    return false;
}

// Attende che un file non sia più in uso
bool WaitForFileAvailability(const std::string& filePath, int maxWaitTimeMs = 30000) {
    int waitTime = 0;
    int sleepIntervalMs = FILE_CHECK_INTERVAL;
    
    WriteToLog("Verifico disponibilità del file: " + filePath, true);
    
    while (waitTime < maxWaitTimeMs) {
        // Verifica se è stato richiesto l'arresto del servizio
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            WriteToLog("Richiesta di arresto ricevuta durante l'attesa del file", true);
            return false;
        }
        
        if (!FileExists(filePath)) {
            WriteToLog("File non trovato durante l'attesa: " + filePath, true);
            return false;
        }
        
        if (!IsFileInUse(filePath)) {
            WriteToLog("File disponibile dopo " + std::to_string(waitTime) + "ms: " + filePath, true);
            return true;
        }
        
        WriteToLog("File in uso, attendo " + std::to_string(sleepIntervalMs) + "ms...", true);
        Sleep(sleepIntervalMs);
        waitTime += sleepIntervalMs;
    }
    
    WriteToLog("Timeout attesa disponibilità file: " + filePath, true);
    return false;
}

// Funzione per leggere il file di configurazione con supporto multi-cartella
bool LoadConfiguration() {
    WriteToLog("Caricamento configurazione da: " + configFile);
    
    // Creare una configurazione predefinita se il file non esiste
    if (!FileExists(configFile)) {
        WriteToLog("File di configurazione non trovato, creazione file predefinito");
        
        // Assicurati che la directory esista
        std::string configDir = configFile.substr(0, configFile.find_last_of("\\/"));
        if (!CreateDirectoryRecursive(configDir)) {
            WriteToLog("ERRORE: Impossibile creare la directory per la configurazione: " + configDir);
            return false;
        }
        
        std::ofstream config(configFile.c_str());
        if (config.is_open()) {
            config << "# PatternTriggerCommand Configuration File - Multi-Folder Support" << std::endl;
            config << "# Autore: Umberto Meglio - Supporto: Claude di Anthropic" << std::endl;
            config << "# Syntax: " << std::endl;
            config << "# [Settings]" << std::endl;
            config << "# DefaultMonitoredFolder=C:\\Monitored (cartella di default per retrocompatibilità)" << std::endl;
            config << "# LogFile=C:\\PTC\\PatternTriggerCommand.log" << std::endl;
            config << "# DetailedLogFile=C:\\PTC\\PatternTriggerCommand_detailed.log" << std::endl;
            config << "# ProcessedFilesDB=C:\\PTC\\PatternTriggerCommand_processed.txt" << std::endl;
            config << "# DetailedLogging=true" << std::endl;
            config << std::endl;
            config << "# [Patterns] - Nuovo formato con supporto multi-cartella" << std::endl;
            config << "# Pattern1=C:\\Folder1|^doc.*\\..*$|C:\\Scripts\\process_doc.bat" << std::endl;
            config << "# Pattern2=C:\\Folder2|^invoice.*\\.pdf$|C:\\Scripts\\process_invoice.bat" << std::endl;
            config << "# Pattern3=^report.*\\.xlsx$|C:\\Scripts\\process_report.bat (usa cartella di default)" << std::endl;
            config << std::endl;
            config << "[Settings]" << std::endl;
            config << "DefaultMonitoredFolder=" << defaultMonitoredFolder << std::endl;
            config << "LogFile=" << logFile << std::endl;
            config << "DetailedLogFile=" << detailedLogFile << std::endl;
            config << "ProcessedFilesDB=" << processedFilesDb << std::endl;
            config << "DetailedLogging=" << (detailedLogging ? "true" : "false") << std::endl;
            config << std::endl;
            config << "[Patterns]" << std::endl;
            config << "Pattern1=C:\\Monitored\\Documents|^doc.*\\..*$|C:\\Scripts\\process_doc.bat" << std::endl;
            config << "Pattern2=C:\\Monitored\\Invoices|^invoice.*\\.pdf$|C:\\Scripts\\process_invoice.bat" << std::endl;
            config << "Pattern3=^report.*\\.xlsx$|C:\\Scripts\\process_report.bat" << std::endl;
            config.close();
            
            WriteToLog("File di configurazione predefinito creato con supporto multi-cartella");
        } else {
            WriteToLog("ERRORE: Impossibile creare il file di configurazione predefinito");
            return false;
        }
    }
    
    // Leggi il file di configurazione
    std::ifstream config(configFile.c_str());
    if (!config.is_open()) {
        WriteToLog("ERRORE: Impossibile aprire il file di configurazione");
        return false;
    }
    
    std::string line;
    std::string currentSection;
    bool hasPatterns = false;
    
    // Svuota il vettore dei pattern
    patternCommandPairs.clear();
    
    while (std::getline(config, line)) {
        // Ignora commenti e linee vuote
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // Rimuovi eventuali commenti di fine riga
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        
        // Rimuovi spazi iniziali e finali
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        
        // Verifica se è una nuova sezione
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }
        
        // Cerca il separatore chiave=valore
        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) {
            continue;  // Ignora linee senza '='
        }
        
        std::string key = line.substr(0, equalPos);
        std::string value = line.substr(equalPos + 1);
        
        // Rimuovi spazi iniziali e finali dalla chiave e dal valore
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        // Processa le impostazioni in base alla sezione
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
            // Nuovo formato: Cartella|RegEx|Comando o RegEx|Comando (usa cartella di default)
            std::vector<std::string> parts;
            std::string temp = value;
            
            // Split sui caratteri |
            size_t pos = 0;
            while ((pos = temp.find('|')) != std::string::npos) {
                parts.push_back(temp.substr(0, pos));
                temp.erase(0, pos + 1);
            }
            if (!temp.empty()) {
                parts.push_back(temp);
            }
            
            std::string folderPath;
            std::string pattern;
            std::string command;
            
            if (parts.size() == 3) {
                // Formato nuovo: Cartella|Pattern|Comando
                folderPath = parts[0];
                pattern = parts[1];
                command = parts[2];
            } else if (parts.size() == 2) {
                // Formato vecchio: Pattern|Comando (usa cartella di default)
                folderPath = defaultMonitoredFolder;
                pattern = parts[0];
                command = parts[1];
            } else {
                WriteToLog("AVVISO: Pattern ignorato, formato non valido: " + value);
                continue;
            }
            
            // Rimuovi spazi dai componenti
            folderPath.erase(0, folderPath.find_first_not_of(" \t"));
            folderPath.erase(folderPath.find_last_not_of(" \t") + 1);
            pattern.erase(0, pattern.find_first_not_of(" \t"));
            pattern.erase(pattern.find_last_not_of(" \t") + 1);
            command.erase(0, command.find_first_not_of(" \t"));
            command.erase(command.find_last_not_of(" \t") + 1);
            
            try {
                patternCommandPairs.emplace_back(folderPath, pattern, command, key);
                hasPatterns = true;
                WriteToLog("Pattern caricato: [" + key + "] Cartella: '" + folderPath + 
                          "', Pattern: '" + pattern + "' -> Comando: '" + command + "'", true);
            } catch (const std::regex_error& e) {
                WriteToLog("ERRORE: Pattern regex non valido '" + pattern + "': " + e.what());
            }
        }
    }
    
    config.close();
    
    // Verifica se abbiamo almeno un pattern
    if (!hasPatterns) {
        WriteToLog("ERRORE: Nessun pattern valido trovato nella configurazione");
        return false;
    }
    
    // Log delle impostazioni caricate
    WriteToLog("Configurazione multi-cartella caricata:");
    WriteToLog("  Cartella di default: " + defaultMonitoredFolder);
    WriteToLog("  File di log: " + logFile);
    WriteToLog("  File di log dettagliato: " + detailedLogFile);
    WriteToLog("  Database file processati: " + processedFilesDb);
    WriteToLog("  Log dettagliato: " + std::string(detailedLogging ? "attivato" : "disattivato"));
    WriteToLog("  Numero di pattern: " + std::to_string(patternCommandPairs.size()));
    
    // Raggruppa i pattern per cartella
    std::map<std::string, int> foldersCount;
    for (const auto& pair : patternCommandPairs) {
        foldersCount[NormalizeFolderPath(pair.folderPath)]++;
    }
    
    WriteToLog("  Cartelle da monitorare: " + std::to_string(foldersCount.size()));
    for (const auto& folder : foldersCount) {
        WriteToLog("    " + folder.first + " (" + std::to_string(folder.second) + " pattern/s)");
    }
    
    return true;
}

// Trova pattern che corrispondono al nome del file in una specifica cartella
// Restituisce un vettore di indici dei pattern che corrispondono
std::vector<int> FindMatchingPatterns(const std::string& filename, const std::string& folderPath) {
    std::vector<int> matchingPatterns;
    std::string normalizedFolder = NormalizeFolderPath(folderPath);
    
    for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
        try {
            // Verifica che il pattern sia associato alla cartella corretta
            if (NormalizeFolderPath(patternCommandPairs[i].folderPath) == normalizedFolder) {
                if (std::regex_match(filename, patternCommandPairs[i].compiledRegex)) {
                    matchingPatterns.push_back(static_cast<int>(i));
                }
            }
        } catch (const std::regex_error& e) {
            WriteToLog("ERRORE durante il match del pattern '" + patternCommandPairs[i].patternRegex + 
                      "': " + e.what());
        }
    }
    return matchingPatterns;
}

// Esegue un comando con un parametro
bool ExecuteCommand(const std::string& command, const std::string& parameter, const std::string& patternName) {
    // Verifica se è stato richiesto l'arresto del servizio
    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
        WriteToLog("Richiesta di arresto ricevuta, annullamento elaborazione comando", true);
        return false;
    }
    
    // Verifica che il comando esista
    if (!FileExists(command)) {
        WriteToLog("ERRORE: Il comando specificato non esiste: " + command);
        return false;
    }
    
    // Usa il percorso completo del file per il tracking
    std::string fullFilePath = parameter;
    
    // Verifica se il file è già stato processato (doppio controllo)
    if (IsFileAlreadyProcessed(fullFilePath)) {
        WriteToLog("SALTATO: File già elaborato in precedenza, non rielaborato: " + fullFilePath);
        return false;
    }
    
    // Attendi che il file sia disponibile (non in uso)
    WriteToLog("Verifico se il file " + fullFilePath + " è disponibile...", true);
    if (!WaitForFileAvailability(parameter)) {
        WriteToLog("ERRORE: File non disponibile dopo il timeout, saltato: " + fullFilePath);
        return false;
    }
    
    // Verifica di nuovo se è stato richiesto l'arresto del servizio
    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
        WriteToLog("Richiesta di arresto ricevuta dopo verifica disponibilità, annullamento", true);
        return false;
    }
    
    std::string commandLine = "\"" + command + "\" \"" + parameter + "\"";
    
    WriteToLog("ELABORAZIONE: Esecuzione comando per pattern [" + patternName + "]: " + commandLine);
    
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    // Creiamo una copia del comando perché CreateProcess modifica la stringa
    char* cmdline = new char[commandLine.length() + 1];
    strcpy(cmdline, commandLine.c_str());
    
    // Eseguiamo il processo
    if (!CreateProcess(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WriteToLog("ERRORE: Errore nell'esecuzione del comando: " + std::to_string(GetLastError()));
        delete[] cmdline;
        return false;
    }
    
    WriteToLog("ELABORAZIONE: Comando avviato con successo [PID: " + std::to_string(pi.dwProcessId) + "]");
    
    // Configura gli handle per attendere o il batch o un evento di arresto
    HANDLE waitHandles[2] = { pi.hProcess, stopEvent };
    
    // Attendiamo il completamento del processo batch con un timeout
    WriteToLog("ELABORAZIONE: Attendo completamento comando con timeout di " + 
               std::to_string(BATCH_TIMEOUT) + "ms", true);
    DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, BATCH_TIMEOUT);
    
    bool success = false;
    
    if (waitResult == WAIT_OBJECT_0) {
        // Il processo è terminato normalmente
        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
            WriteToLog("COMPLETATO: Comando completato con codice di uscita: " + std::to_string(exitCode));
        } else {
            WriteToLog("COMPLETATO: Comando completato ma impossibile ottenere il codice di uscita");
        }
        // Marca il file come processato usando il percorso completo
        MarkFileAsProcessed(fullFilePath);
        success = true;
    } else if (waitResult == WAIT_OBJECT_0 + 1) {
        // L'evento stopEvent è stato segnalato
        WriteToLog("ANNULLATO: Comando interrotto a causa della richiesta di arresto");
        // Termina forzatamente il processo batch
        TerminateProcess(pi.hProcess, 1);
        success = false;
    } else if (waitResult == WAIT_TIMEOUT) {
        WriteToLog("ATTENZIONE: Timeout nell'attesa del comando, potrebbe essere ancora in esecuzione");
        // Termina forzatamente il processo batch
        TerminateProcess(pi.hProcess, 1);
        // In caso di timeout, consideriamo comunque il file come processato
        // per evitare di elaborarlo nuovamente
        MarkFileAsProcessed(fullFilePath);
        success = true;
    } else {
        WriteToLog("ERRORE: Errore nell'attesa del comando: " + std::to_string(GetLastError()));
        success = false;
    }
    
    // Chiudiamo gli handle del processo e del thread
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    delete[] cmdline;
    
    // Attendi un po' prima di continuare per assicurarti che eventuali operazioni di pulizia siano completate
    WriteToLog("ELABORAZIONE: Attendo " + std::to_string(MONITORING_RESTART_DELAY) + 
               "ms prima di riprendere il monitoraggio", true);
    
    // Verifica se è stato richiesto l'arresto del servizio prima di dormire
    if (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        Sleep(MONITORING_RESTART_DELAY);
    }
    
    return success;
}

// Configura il monitoraggio di una directory specifica
HANDLE SetupDirectoryMonitoring(const std::string& folderPath) {
    WriteToLog("Configurazione monitoraggio directory: " + folderPath);
    
    HANDLE hDir = CreateFile(
        folderPath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,  // FLAG_OVERLAPPED per I/O asincrono
        NULL
    );
    
    if (hDir == INVALID_HANDLE_VALUE) {
        WriteToLog("Errore nell'apertura della directory: " + folderPath + " - " + std::to_string(GetLastError()));
        return INVALID_HANDLE_VALUE;
    }
    
    WriteToLog("Directory aperta con successo: " + folderPath + ", handle: " + std::to_string((DWORD)hDir), true);
    return hDir;
}

// Esegue una scansione completa delle directory e processa i file trovati
void ScanDirectoriesForFiles() {
    WriteToLog("Scansione iniziale di tutte le cartelle monitorate per file esistenti...");
    int totalFilesFound = 0;
    int totalMatchingFilesFound = 0;
    int totalFilesProcessed = 0;
    int totalFilesSkipped = 0;
    
    // Raggruppa i pattern per cartella per efficienza
    std::map<std::string, std::vector<int>> folderPatterns;
    for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
        std::string normalizedFolder = NormalizeFolderPath(patternCommandPairs[i].folderPath);
        folderPatterns[normalizedFolder].push_back(static_cast<int>(i));
    }
    
    // Scansiona ogni cartella
    for (const auto& folderGroup : folderPatterns) {
        std::string folderPath = patternCommandPairs[folderGroup.second[0]].folderPath; // Prendi il percorso originale
        WriteToLog("Scansione cartella: " + folderPath + " (con " + std::to_string(folderGroup.second.size()) + " pattern/s)");
        
        int filesFound = 0;
        int matchingFilesFound = 0;
        int filesProcessed = 0;
        int filesSkipped = 0;
        
        // Apri la directory
        std::string searchPath = folderPath + "\\*.*";
        WIN32_FIND_DATA findData;
        HANDLE hFind = FindFirstFile(searchPath.c_str(), &findData);
        
        if (hFind == INVALID_HANDLE_VALUE) {
            WriteToLog("Errore nell'apertura directory per scansione: " + folderPath + " - " + std::to_string(GetLastError()));
            continue;
        }
        
        do {
            // Controlla se è una richiesta di arresto
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                WriteToLog("Richiesta di arresto durante la scansione iniziale");
                FindClose(hFind);
                return;
            }
            
            // Salta le directory (incluso . e ..)
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            
            // Incrementa il contatore dei file totali
            filesFound++;
            
            std::string filename = findData.cFileName;
            std::string fullPath = folderPath + "\\" + filename;
            
            // Controlla se il file corrisponde a uno dei pattern per questa cartella
            std::vector<int> matchingPatterns = FindMatchingPatterns(filename, folderPath);
            if (!matchingPatterns.empty()) {
                matchingFilesFound++;
                
                // Controlla se è già stato processato (usando il percorso completo)
                if (!IsFileAlreadyProcessed(fullPath)) {
                    WriteToLog("File corrispondente al pattern trovato (da elaborare): " + fullPath);
                    
                    // Elabora il file con tutti i comandi associati ai pattern corrispondenti
                    for (int patternIndex : matchingPatterns) {
                        if (ExecuteCommand(patternCommandPairs[patternIndex].command, fullPath, 
                                         patternCommandPairs[patternIndex].patternName)) {
                            filesProcessed++;
                        }
                        
                        // Verifica se è stato richiesto l'arresto del servizio
                        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                            WriteToLog("Richiesta di arresto durante elaborazione scansione");
                            FindClose(hFind);
                            return;
                        }
                    }
                } else {
                    // File già processato
                    if (ShouldLogIgnoredFile(fullPath)) {
                        WriteToLog("IGNORATO: File già processato: " + fullPath);
                    }
                    filesSkipped++;
                }
            }
        } while (FindNextFile(hFind, &findData));
        
        // Chiudi l'handle di ricerca
        FindClose(hFind);
        
        WriteToLog("Scansione cartella " + folderPath + " completata - Trovati: " + std::to_string(filesFound) + 
                   ", Corrispondenti: " + std::to_string(matchingFilesFound) +
                   ", Elaborati: " + std::to_string(filesProcessed) + 
                   ", Ignorati: " + std::to_string(filesSkipped));
        
        // Aggiorna i totali
        totalFilesFound += filesFound;
        totalMatchingFilesFound += matchingFilesFound;
        totalFilesProcessed += filesProcessed;
        totalFilesSkipped += filesSkipped;
    }
    
    WriteToLog("Scansione iniziale globale completata - Totale trovati: " + std::to_string(totalFilesFound) + 
               ", Totale corrispondenti: " + std::to_string(totalMatchingFilesFound) +
               ", Totale elaborati: " + std::to_string(totalFilesProcessed) + 
               ", Totale ignorati: " + std::to_string(totalFilesSkipped));
}

// Funzione per gestire gli eventi di console (CTRL+C, ecc.)
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        std::cout << "\nRichiesta interruzione, attendi il completamento..." << std::endl;
        if (stopEvent != NULL) {
            SetEvent(stopEvent);
        }
        return TRUE;  // Abbiamo gestito l'evento
    }
    return FALSE;  // Non abbiamo gestito l'evento
}

// Funzione principale del servizio con supporto multi-cartella
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam) {
    WriteToLog("Servizio multi-cartella avviato - verificando requisiti...");
    
    // Carica la configurazione
    if (!LoadConfiguration()) {
        WriteToLog("ERRORE FATALE: Impossibile caricare la configurazione");
        return 1;
    }
    
    // Assicura che la directory degli script esista
    std::string scriptDir = "C:\\Scripts";
    if (!FileExists(scriptDir)) {
        CreateDirectoryRecursive(scriptDir);
    }
    
    // Carica i file già processati
    LoadProcessedFiles();
    
    // Pulisci la cache dei file ignorati all'avvio
    recentlyIgnoredFiles.clear();
    
    // Verifica che tutte le directory necessarie esistano e le crea se necessario
    std::set<std::string> uniqueFolders;
    for (const auto& pair : patternCommandPairs) {
        uniqueFolders.insert(pair.folderPath);
    }
    
    for (const auto& folder : uniqueFolders) {
        if (!EnsureDirectoryExists(folder)) {
            WriteToLog("ERRORE FATALE: Impossibile accedere o creare la directory di monitoraggio: " + folder);
            return 1;
        }
    }
    
    WriteToLog("Tutti i requisiti verificati, avvio monitoraggio multi-cartella...");
    
    // Scansione iniziale per processare i file esistenti in tutte le cartelle
    ScanDirectoriesForFiles();
    
    // Se è stata richiesta la terminazione durante la scansione iniziale, esci
    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
        WriteToLog("Terminazione richiesta durante o dopo la scansione iniziale");
        return 0;
    }
    
    // Inizializza le strutture di monitoraggio per ogni cartella unica
    monitoredFolders.clear();
    for (const auto& folder : uniqueFolders) {
        std::string normalizedFolder = NormalizeFolderPath(folder);
        FolderMonitorInfo& info = monitoredFolders[normalizedFolder];
        info.hDir = INVALID_HANDLE_VALUE;
        info.active = false;
        
        // Trova tutti i pattern associati a questa cartella
        for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
            if (NormalizeFolderPath(patternCommandPairs[i].folderPath) == normalizedFolder) {
                info.patternIndices.push_back(static_cast<int>(i));
            }
        }
        
        WriteToLog("Configurato monitoraggio per cartella: " + folder + " (" + 
                   std::to_string(info.patternIndices.size()) + " pattern/s associati)", true);
    }
    
    bool running = true;
    
    // Inizializza un timer per pulire periodicamente la cache dei file ignorati
    DWORD lastCacheCleanupTime = GetTickCount();
    
    WriteToLog("Avvio ciclo di monitoraggio principale multi-cartella");
    
    while (running) {
        // Verifica se è richiesta la terminazione del servizio
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            WriteToLog("Richiesta di arresto ricevuta, terminazione monitoraggio");
            running = false;
            break;
        }
        
        // Controlla se è ora di pulire la cache dei file ignorati
        DWORD currentTime = GetTickCount();
        if (currentTime - lastCacheCleanupTime > CACHE_CLEANUP_INTERVAL) {
            CleanupRecentlyIgnoredFiles();
            lastCacheCleanupTime = currentTime;
        }
        
        // Configura il monitoraggio per tutte le cartelle
        std::vector<HANDLE> waitHandles;
        std::vector<std::string> handleToFolder;
        
        waitHandles.push_back(stopEvent); // Primo handle è sempre l'evento di stop
        handleToFolder.push_back(""); // Placeholder per stopEvent
        
        for (auto& folderPair : monitoredFolders) {
            const std::string& normalizedFolder = folderPair.first;
            FolderMonitorInfo& info = folderPair.second;
            
            // Trova il percorso originale
            std::string originalFolder = patternCommandPairs[info.patternIndices[0]].folderPath;
            
            // Se il handle della directory non è valido, configuralo
            if (info.hDir == INVALID_HANDLE_VALUE) {
                info.hDir = SetupDirectoryMonitoring(originalFolder);
                if (info.hDir == INVALID_HANDLE_VALUE) {
                    WriteToLog("Impossibile configurare il monitoraggio della directory: " + originalFolder);
                    continue;
                }
            }
            
            // Inizializza la struttura overlapped per I/O asincrono
            ZeroMemory(&info.overlapped, sizeof(info.overlapped));
            info.overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            
            if (!info.overlapped.hEvent) {
                WriteToLog("Errore nella creazione dell'evento per I/O asincrono per cartella: " + originalFolder + 
                           " - " + std::to_string(GetLastError()));
                if (info.hDir != INVALID_HANDLE_VALUE) {
                    CloseHandle(info.hDir);
                    info.hDir = INVALID_HANDLE_VALUE;
                }
                continue;
            }
            
            WriteToLog("Avvio ReadDirectoryChangesW per cartella: " + originalFolder, true);
            
            // Imposta notifiche per modifiche alla directory in modo asincrono
            BOOL success = ReadDirectoryChangesW(
                info.hDir,
                info.buffer,
                sizeof(info.buffer),
                FALSE,  // Non monitorare sottocartelle
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                NULL,
                &info.overlapped,  // Operazione asincrona
                NULL
            );
            
            if (!success && GetLastError() != ERROR_IO_PENDING) {
                DWORD error = GetLastError();
                WriteToLog("Errore in ReadDirectoryChangesW per cartella " + originalFolder + ": " + std::to_string(error));
                CloseHandle(info.overlapped.hEvent);
                info.overlapped.hEvent = NULL;
                
                if (info.hDir != INVALID_HANDLE_VALUE) {
                    CloseHandle(info.hDir);
                    info.hDir = INVALID_HANDLE_VALUE;
                }
                continue;
            }
            
            // Aggiungi l'handle alla lista di attesa
            waitHandles.push_back(info.overlapped.hEvent);
            handleToFolder.push_back(normalizedFolder);
            info.active = true;
        }
        
        // Se non ci sono cartelle attive, attendi e riprova
        if (waitHandles.size() <= 1) {
            WriteToLog("Nessuna cartella attiva per il monitoraggio, attendo 5 secondi prima di riprovare");
            Sleep(5000);
            continue;
        }
        
        // Attendi eventi da qualsiasi cartella o il segnale di arresto
        DWORD waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(waitHandles.size()), 
            waitHandles.data(), 
            FALSE, 
            DIR_CHANGES_WAIT_TIMEOUT
        );
        
        if (waitResult == WAIT_OBJECT_0) {
            // Evento di arresto segnalato
            WriteToLog("Richiesta di arresto durante l'attesa di modifiche alle directory");
            running = false;
            break;
        }
        else if (waitResult >= WAIT_OBJECT_0 + 1 && waitResult < WAIT_OBJECT_0 + waitHandles.size()) {
            // Evento di modifica a una directory segnalato
            size_t folderIndex = waitResult - WAIT_OBJECT_0;
            std::string normalizedFolder = handleToFolder[folderIndex];
            FolderMonitorInfo& info = monitoredFolders[normalizedFolder];
            std::string originalFolder = patternCommandPairs[info.patternIndices[0]].folderPath;
            
            DWORD bytesTransferred = 0;
            if (!GetOverlappedResult(info.hDir, &info.overlapped, &bytesTransferred, FALSE)) {
                DWORD error = GetLastError();
                WriteToLog("Errore in GetOverlappedResult per cartella " + originalFolder + ": " + std::to_string(error));
                
                // Pulisci l'handle problematico
                if (info.overlapped.hEvent) {
                    CloseHandle(info.overlapped.hEvent);
                    info.overlapped.hEvent = NULL;
                }
                
                if (error == ERROR_OPERATION_ABORTED) {
                    if (info.hDir != INVALID_HANDLE_VALUE) {
                        CloseHandle(info.hDir);
                        info.hDir = INVALID_HANDLE_VALUE;
                    }
                }
                
                info.active = false;
                continue;
            }
            
            if (bytesTransferred == 0) {
                WriteToLog("Nessuna modifica rilevata per cartella: " + originalFolder, true);
                if (info.overlapped.hEvent) {
                    CloseHandle(info.overlapped.hEvent);
                    info.overlapped.hEvent = NULL;
                }
                info.active = false;
                continue;
            }
            
            WriteToLog("Modifiche rilevate nella cartella: " + originalFolder, true);
            
            // Processa le notifiche per questa cartella
            FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)info.buffer;
            bool fileProcessed = false;
            
            do {
                // Verifica se è richiesta la terminazione del servizio
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                    WriteToLog("Richiesta di arresto durante il processamento delle notifiche");
                    running = false;
                    break;
                }
                
                // Converte il nome del file da WCHAR a char
                char filename[MAX_PATH];
                int filenameLength = WideCharToMultiByte(
                    CP_ACP,
                    0,
                    fni->FileName,
                    fni->FileNameLength / sizeof(WCHAR),
                    filename,
                    sizeof(filename),
                    NULL,
                    NULL
                );
                filename[filenameLength] = '\0';
                
                std::string strFilename(filename);
                std::string actionName;
                
                // Determina il tipo di azione
                switch (fni->Action) {
                    case FILE_ACTION_ADDED:
                        actionName = "ADDED";
                        break;
                    case FILE_ACTION_REMOVED:
                        actionName = "REMOVED";
                        break;
                    case FILE_ACTION_MODIFIED:
                        actionName = "MODIFIED";
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        actionName = "RENAMED_OLD";
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        actionName = "RENAMED_NEW";
                        break;
                    default:
                        actionName = "UNKNOWN(" + std::to_string(fni->Action) + ")";
                }
                
                WriteToLog("Notifica file in " + originalFolder + ": " + actionName + " - " + strFilename, true);
                
                // Verifica se il file corrisponde a uno dei pattern configurati per questa cartella
                std::vector<int> matchingPatterns = FindMatchingPatterns(strFilename, originalFolder);
                
                // Verifica se il file deve essere processato
                if ((fni->Action == FILE_ACTION_ADDED || 
                     fni->Action == FILE_ACTION_RENAMED_NEW_NAME || 
                     fni->Action == FILE_ACTION_MODIFIED) && 
                    !matchingPatterns.empty()) {
                    
                    std::string fullPath = originalFolder + "\\" + strFilename;
                    
                    // Verifica se il file è già stato processato - migliorato per evitare log ripetitivi
                    if (IsFileAlreadyProcessed(fullPath)) {
                        // Log solo se non è stato recentemente ignorato
                        if (ShouldLogIgnoredFile(fullPath)) {
                            WriteToLog("IGNORATO: File già elaborato in precedenza: " + fullPath);
                        }
                        
                        // Passa al record successivo
                        if (fni->NextEntryOffset != 0) {
                            fni = (FILE_NOTIFY_INFORMATION*)((BYTE*)fni + fni->NextEntryOffset);
                            continue;
                        } else {
                            break;
                        }
                    }
                    
                    WriteToLog("File corrispondente a " + std::to_string(matchingPatterns.size()) + 
                              " pattern/s rilevato: " + fullPath + " (azione: " + actionName + ")");
                    
                    // Esegui tutti i comandi associati ai pattern corrispondenti
                    for (int patternIndex : matchingPatterns) {
                        WriteToLog("Elaborazione con pattern [" + patternCommandPairs[patternIndex].patternName + 
                                  "]: " + patternCommandPairs[patternIndex].patternRegex);
                        
                        if (ExecuteCommand(patternCommandPairs[patternIndex].command, fullPath, 
                                         patternCommandPairs[patternIndex].patternName)) {
                            fileProcessed = true;
                        }
                        
                        // Verifica se è stato richiesto l'arresto del servizio
                        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                            WriteToLog("Richiesta di arresto ricevuta dopo elaborazione comando");
                            running = false;
                            break;
                        }
                    }
                    
                    if (!running) break;
                }
                
                // Passa al record successivo
                if (fni->NextEntryOffset == 0) {
                    break;
                }
                
                fni = (FILE_NOTIFY_INFORMATION*)((BYTE*)fni + fni->NextEntryOffset);
            } while (true);
            
            // Pulisci l'handle dell'evento per questa cartella
            if (info.overlapped.hEvent) {
                CloseHandle(info.overlapped.hEvent);
                info.overlapped.hEvent = NULL;
            }
            info.active = false;
            
            // Se è stato elaborato un file, reinizializza tutti i monitoraggi
            if (fileProcessed) {
                WriteToLog("File elaborato, reinizializzo monitoraggio cartelle", true);
                // Chiudi tutti i handle attivi
                for (auto& folderPair : monitoredFolders) {
                    FolderMonitorInfo& folderInfo = folderPair.second;
                    if (folderInfo.hDir != INVALID_HANDLE_VALUE) {
                        CancelIoEx(folderInfo.hDir, NULL);
                        CloseHandle(folderInfo.hDir);
                        folderInfo.hDir = INVALID_HANDLE_VALUE;
                    }
                    if (folderInfo.overlapped.hEvent) {
                        CloseHandle(folderInfo.overlapped.hEvent);
                        folderInfo.overlapped.hEvent = NULL;
                    }
                    folderInfo.active = false;
                }
            }
        }
        else if (waitResult == WAIT_TIMEOUT) {
            // Timeout sull'attesa delle modifiche, continua il loop
            WriteToLog("Timeout nell'attesa di modifiche alle directory", true);
            
            // Pulisci tutti gli handle overlapped
            for (auto& folderPair : monitoredFolders) {
                FolderMonitorInfo& folderInfo = folderPair.second;
                if (folderInfo.active && folderInfo.hDir != INVALID_HANDLE_VALUE) {
                    CancelIoEx(folderInfo.hDir, &folderInfo.overlapped);
                }
                if (folderInfo.overlapped.hEvent) {
                    CloseHandle(folderInfo.overlapped.hEvent);
                    folderInfo.overlapped.hEvent = NULL;
                }
                folderInfo.active = false;
            }
        }
        else {
            // Errore nell'attesa
            WriteToLog("Errore durante l'attesa: " + std::to_string(GetLastError()));
            
            // Pulisci tutti gli handle
            for (auto& folderPair : monitoredFolders) {
                FolderMonitorInfo& folderInfo = folderPair.second;
                if (folderInfo.overlapped.hEvent) {
                    CloseHandle(folderInfo.overlapped.hEvent);
                    folderInfo.overlapped.hEvent = NULL;
                }
                if (folderInfo.hDir != INVALID_HANDLE_VALUE) {
                    CloseHandle(folderInfo.hDir);
                    folderInfo.hDir = INVALID_HANDLE_VALUE;
                }
                folderInfo.active = false;
            }
            
            // Pausa prima di riprovare
            Sleep(1000);
        }
    }
    
    // Salva il database dei file processati
    SaveProcessedFiles();
    
    // Pulisci tutte le risorse
    for (auto& folderPair : monitoredFolders) {
        FolderMonitorInfo& folderInfo = folderPair.second;
        if (folderInfo.hDir != INVALID_HANDLE_VALUE) {
            CloseHandle(folderInfo.hDir);
        }
        if (folderInfo.overlapped.hEvent) {
            CloseHandle(folderInfo.overlapped.hEvent);
        }
    }
    
    WriteToLog("Thread del servizio multi-cartella terminato");
    return 0;
}

// Funzione chiamata quando il servizio viene arrestato
void WINAPI ServiceCtrlHandler(DWORD ctrlCode) {
    switch (ctrlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            WriteToLog("Richiesta di arresto del servizio ricevuta");
            
            // Aggiorna lo stato del servizio
            serviceStatus.dwControlsAccepted = 0;
            serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            serviceStatus.dwWin32ExitCode = 0;
            serviceStatus.dwCheckPoint = 4;
            serviceStatus.dwWaitHint = SERVICE_SHUTDOWN_TIMEOUT; 
            
            if (SetServiceStatus(serviceStatusHandle, &serviceStatus) == FALSE) {
                WriteToLog("SetServiceStatus error");
            }
            
            // Segnala al thread di servizio di terminare
            if (stopEvent != NULL) {
                SetEvent(stopEvent);
                // Attesa breve per assicurarsi che il segnale sia ricevuto
                Sleep(500);
            } else {
                WriteToLog("ERRORE: stopEvent è NULL durante la richiesta di arresto!");
            }
            break;
            
        default:
            break;
    }
}

// Punto di ingresso del servizio
void WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    // Registra la funzione di controllo del servizio
    serviceStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);
    
    if (serviceStatusHandle == NULL) {
        WriteToLog("RegisterServiceCtrlHandler failed");
        return;
    }
    
    // Inizializza la struttura dello stato del servizio
    ZeroMemory(&serviceStatus, sizeof(serviceStatus));
    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    serviceStatus.dwWin32ExitCode = 0;
    serviceStatus.dwServiceSpecificExitCode = 0;
    serviceStatus.dwCheckPoint = 0;
    serviceStatus.dwWaitHint = 0;
    
    // Aggiorna lo stato del servizio a "starting"
    if (SetServiceStatus(serviceStatusHandle, &serviceStatus) == FALSE) {
        WriteToLog("SetServiceStatus error");
    }
    
    // Crea un evento per il controllo del servizio
    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stopEvent == NULL) {
        WriteToLog("CreateEvent failed");
        
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        serviceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(serviceStatusHandle, &serviceStatus);
        return;
    }
    
    // Avvia il thread del servizio
    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    
    if (hThread == NULL) {
        WriteToLog("CreateThread failed");
        
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        serviceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(serviceStatusHandle, &serviceStatus);
        return;
    }
    
    // Dichiara il servizio come avviato
    serviceStatus.dwCurrentState = SERVICE_RUNNING;
    serviceStatus.dwCheckPoint = 0;
    serviceStatus.dwWaitHint = 0;
    
    if (SetServiceStatus(serviceStatusHandle, &serviceStatus) == FALSE) {
        WriteToLog("SetServiceStatus error");
    }
    
    // Attendi che il thread del servizio termini - senza timeout
    WaitForSingleObject(hThread, INFINITE);
    
    // Pulisci le risorse
    CloseHandle(stopEvent);
    CloseHandle(hThread);
    
    // Imposta lo stato del servizio a "stopped"
    serviceStatus.dwCurrentState = SERVICE_STOPPED;
    serviceStatus.dwCheckPoint = 0;
    serviceStatus.dwWaitHint = 0;
    
    if (SetServiceStatus(serviceStatusHandle, &serviceStatus) == FALSE) {
        WriteToLog("SetServiceStatus error");
    }
    
    WriteToLog("Servizio terminato");
}

// Funzione principale del programma
int main(int argc, char* argv[]) {
    // Inizio con un messaggio di log
    std::string configFileStr = DEFAULT_CONFIG_FILE;
    std::string baseDir = configFileStr.substr(0, configFileStr.find_last_of("\\/"));
    CreateDirectoryRecursive(baseDir);
    SetCurrentDirectory(baseDir.c_str());
    
    WriteToLog("-------------------------", false);
    WriteToLog("Avvio applicazione PatternTriggerCommand Multi-Folder");
    WriteToLog("Autore: Umberto Meglio - Supporto: Claude di Anthropic");
    
    // Controlla se ci sono parametri per il log dettagliato
    if (argc > 1) {
        if (strcmp(argv[argc-1], "nolog") == 0) {
            detailedLogging = false;
            WriteToLog("Log dettagliato disattivato");
        } else if (strcmp(argv[argc-1], "detaillog") == 0) {
            detailedLogging = true;
            WriteToLog("Log dettagliato attivato");
        }
    }
    
    // Se specificato, carica il file di configurazione alternativo
    if (argc > 2 && strcmp(argv[1], "config") == 0) {
        configFile = argv[2];
        WriteToLog("Utilizzo file di configurazione alternativo: " + std::string(argv[2]));
    }
    
    // Se il programma viene eseguito con l'argomento "install", installa il servizio
    if (argc > 1 && strcmp(argv[1], "install") == 0) {
        WriteToLog("Richiesta installazione servizio");
        
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
        
        if (schSCManager == NULL) {
            DWORD err = GetLastError();
            std::string errMsg = "OpenSCManager failed: " + std::to_string(err);
            WriteToLog(errMsg);
            std::cerr << errMsg << std::endl;
            return 1;
        }
        
        // Ottieni il percorso completo dell'eseguibile
        char path[MAX_PATH];
        GetModuleFileName(NULL, path, MAX_PATH);
        WriteToLog("Percorso eseguibile: " + std::string(path));
        
        // Crea il servizio
        SC_HANDLE schService = CreateService(
            schSCManager,
            SERVICE_NAME,
            SERVICE_DISPLAY_NAME,
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            path,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL
        );
        
        if (schService == NULL) {
            DWORD err = GetLastError();
            std::string errMsg = "CreateService failed: " + std::to_string(err);
            WriteToLog(errMsg);
            std::cerr << errMsg << std::endl;
            CloseServiceHandle(schSCManager);
            return 1;
        }
        
        // Tenta di impostare la descrizione del servizio
        WriteToLog("Impostazione descrizione servizio");
        
        // Definisci la struttura necessaria
        SERVICE_DESCRIPTION_STRUCT sd;
        sd.lpDescription = const_cast<LPSTR>(SERVICE_DESCRIPTION);
        
        // Usa la nostra funzione personalizzata invece di ChangeServiceConfig2
        BOOL result = MyChangeServiceConfig2(
            schService,
            SERVICE_CONFIG_DESCRIPTION,
            &sd
        );
        
        if (result) {
            WriteToLog("Descrizione servizio impostata con successo");
        } else {
            WriteToLog("Impossibile impostare la descrizione del servizio: " + 
                       std::to_string(GetLastError()));
        }
        
        WriteToLog("Servizio multi-cartella installato con successo");
        std::cout << "Servizio multi-cartella installato con successo." << std::endl;
        
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
    }
    // Se il programma viene eseguito con l'argomento "uninstall", disinstalla il servizio
    else if (argc > 1 && strcmp(argv[1], "uninstall") == 0) {
        WriteToLog("Richiesta disinstallazione servizio");
        
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        
        if (schSCManager == NULL) {
            DWORD err = GetLastError();
            std::string errMsg = "OpenSCManager failed: " + std::to_string(err);
            WriteToLog(errMsg);
            std::cerr << errMsg << std::endl;
            return 1;
        }
        
        SC_HANDLE schService = OpenService(schSCManager, SERVICE_NAME, DELETE);
        
        if (schService == NULL) {
            DWORD err = GetLastError();
            std::string errMsg = "OpenService failed: " + std::to_string(err);
            WriteToLog(errMsg);
            std::cerr << errMsg << std::endl;
            CloseServiceHandle(schSCManager);
            return 1;
        }
        
        if (!DeleteService(schService)) {
            DWORD err = GetLastError();
            std::string errMsg = "DeleteService failed: " + std::to_string(err);
            WriteToLog(errMsg);
            std::cerr << errMsg << std::endl;
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
            return 1;
        }
        
        WriteToLog("Servizio disinstallato con successo");
        std::cout << "Servizio disinstallato con successo." << std::endl;
        
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
    }
    // Se il programma viene eseguito con l'argomento "test", esegue un test
    else if (argc > 1 && strcmp(argv[1], "test") == 0) {
        WriteToLog("Modalità test multi-cartella");
        std::cout << "Test del servizio multi-cartella in modalità console..." << std::endl;
        
        // Carica la configurazione
        if (!LoadConfiguration()) {
            WriteToLog("ERRORE: Impossibile caricare la configurazione");
            return 1;
        }
        
        std::cout << "Configurazione multi-cartella caricata:" << std::endl;
        std::cout << "  Cartella di default: " << defaultMonitoredFolder << std::endl;
        std::cout << "  File di log: " << logFile << std::endl;
        std::cout << "  Pattern configurati: " << patternCommandPairs.size() << std::endl;
        
        // Raggruppa i pattern per cartella per la visualizzazione
        std::map<std::string, std::vector<int>> folderPatterns;
        for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
            folderPatterns[patternCommandPairs[i].folderPath].push_back(static_cast<int>(i));
        }
        
        std::cout << "  Cartelle monitorate: " << folderPatterns.size() << std::endl;
        for (const auto& folderGroup : folderPatterns) {
            std::cout << "    Cartella: " << folderGroup.first << " (" << folderGroup.second.size() << " pattern/s)" << std::endl;
            for (int patternIndex : folderGroup.second) {
                std::cout << "      Pattern [" << patternCommandPairs[patternIndex].patternName << "]: '" 
                          << patternCommandPairs[patternIndex].patternRegex 
                          << "' -> '" << patternCommandPairs[patternIndex].command << "'" << std::endl;
            }
        }
        
        std::cout << "\nIntervallo controllo file: " << FILE_CHECK_INTERVAL << "ms" << std::endl;
        std::cout << "Intervallo riavvio monitoraggio: " << MONITORING_RESTART_DELAY << "ms" << std::endl;
        std::cout << "Intervallo pulizia cache: " << CACHE_CLEANUP_INTERVAL << "ms" << std::endl;
        std::cout << "Timeout arresto servizio: " << SERVICE_SHUTDOWN_TIMEOUT << "ms" << std::endl;
        
        std::cout << "\nPremi CTRL+C per terminare il test..." << std::endl;
        
        // Crea un evento per simulare la richiesta di arresto del servizio
        stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (stopEvent == NULL) {
            WriteToLog("CreateEvent failed");
            return 1;
        }
        
        // Gestione CTRL+C per terminare il test
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        
        // Esegui il thread di servizio direttamente
        ServiceWorkerThread(NULL);
        
        // Pulisci le risorse
        CloseHandle(stopEvent);
    }
    // Se il programma viene eseguito con l'argomento "reset", resetta il database dei file processati
    else if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        // Carica la configurazione
        LoadConfiguration();
        
        WriteToLog("Reset del database dei file processati");
        std::ofstream file(processedFilesDb.c_str(), std::ios::trunc);
        if (file.is_open()) {
            file.close();
            WriteToLog("Database dei file processati azzerato con successo");
            std::cout << "Database dei file processati azzerato con successo." << std::endl;
        } else {
            WriteToLog("Errore nell'azzeramento del database");
            std::cerr << "Errore nell'azzeramento del database." << std::endl;
        }
    }
    // Opzione per verificare quanti file sono stati processati
    else if (argc > 1 && strcmp(argv[1], "status") == 0) {
        // Carica la configurazione
        LoadConfiguration();
        
        LoadProcessedFiles();
        std::cout << "PatternTriggerCommand Multi-Folder - Stato attuale\n";
        std::cout << "--------------------------------------------\n";
        std::cout << "File già processati: " << processedFiles.size() << std::endl;
        
        // Controlla se il servizio è in esecuzione
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (schSCManager) {
            SC_HANDLE schService = OpenService(schSCManager, SERVICE_NAME, SERVICE_QUERY_STATUS);
            if (schService) {
                SERVICE_STATUS_PROCESS ssp;
                DWORD bytesNeeded;
                if (QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
                    std::cout << "Stato del servizio: ";
                    switch (ssp.dwCurrentState) {
                        case SERVICE_STOPPED:
                            std::cout << "FERMATO";
                            break;
                        case SERVICE_START_PENDING:
                            std::cout << "AVVIO IN CORSO";
                            break;
                        case SERVICE_STOP_PENDING:
                            std::cout << "ARRESTO IN CORSO";
                            break;
                        case SERVICE_RUNNING:
                            std::cout << "IN ESECUZIONE (PID: " << ssp.dwProcessId << ")";
                            break;
                        default:
                            std::cout << "STATO SCONOSCIUTO (" << ssp.dwCurrentState << ")";
                    }
                    std::cout << std::endl;
                }
                CloseServiceHandle(schService);
            } else {
                std::cout << "Stato del servizio: NON INSTALLATO" << std::endl;
            }
            CloseServiceHandle(schSCManager);
        }
        
        // Visualizza la configurazione corrente
        std::cout << "\nConfigurazione multi-cartella:\n";
        std::cout << "  File di configurazione: " << configFile << std::endl;
        std::cout << "  Cartella di default: " << defaultMonitoredFolder << 
                  (FileExists(defaultMonitoredFolder) ? " (OK)" : " (NON ESISTE!)") << std::endl;
        
        // Raggruppa i pattern per cartella
        std::map<std::string, std::vector<int>> folderPatterns;
        for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
            folderPatterns[patternCommandPairs[i].folderPath].push_back(static_cast<int>(i));
        }
        
        std::cout << "  Cartelle monitorate: " << folderPatterns.size() << std::endl;
        for (const auto& folderGroup : folderPatterns) {
            std::cout << "    Cartella: " << folderGroup.first << 
                      (FileExists(folderGroup.first) ? " (OK)" : " (NON ESISTE!)") << 
                      " - " << folderGroup.second.size() << " pattern/s" << std::endl;
            
            for (int patternIndex : folderGroup.second) {
                std::cout << "      [" << patternCommandPairs[patternIndex].patternName << "] '" 
                          << patternCommandPairs[patternIndex].patternRegex 
                          << "' -> '" << patternCommandPairs[patternIndex].command 
                          << (FileExists(patternCommandPairs[patternIndex].command) ? " (OK)" : " (NON ESISTE!)") << "'" << std::endl;
            }
        }
        
        // Visualizza ultimi eventi dal log
        std::cout << "\nUltimi eventi di log:\n";
        std::ifstream logFileStream(logFile.c_str());
        std::string line;
        std::vector<std::string> lastLines;
        while (std::getline(logFileStream, line)) {
            lastLines.push_back(line);
            if (lastLines.size() > 10) {
                lastLines.erase(lastLines.begin());
            }
        }
        for (const auto& logLine : lastLines) {
            std::cout << logLine << std::endl;
        }
    }
    // Meccanismo per riprocessare un singolo file (ora richiede anche la cartella)
    else if (argc > 3 && strcmp(argv[1], "reprocess") == 0) {
        // Carica la configurazione
        LoadConfiguration();
        
        std::string folderPath = argv[2];
        std::string filename = argv[3];
        WriteToLog("Richiesta di riprocessare manualmente il file: " + filename + " nella cartella: " + folderPath);
        
        // Controlla se il file esiste
        std::string fullPath = folderPath + "\\" + filename;
        if (FileExists(fullPath)) {
            // Verifica se è nel database
            LoadProcessedFiles();
            if (IsFileAlreadyProcessed(fullPath)) {
                // Rimuovi dal database
                processedFiles.erase(fullPath);
                SaveProcessedFiles();
                WriteToLog("File rimosso dal database dei processati: " + fullPath);
            }
            
            // Trova i pattern corrispondenti
            std::vector<int> matchingPatterns = FindMatchingPatterns(filename, folderPath);
            if (!matchingPatterns.empty()) {
                // Eseguiamo tutti i comandi corrispondenti
                WriteToLog("Riprocessamento manuale del file: " + fullPath + " con " + 
                          std::to_string(matchingPatterns.size()) + " pattern/s");
                
                for (int patternIndex : matchingPatterns) {
                    ExecuteCommand(patternCommandPairs[patternIndex].command, fullPath, 
                                 patternCommandPairs[patternIndex].patternName);
                }
                
                std::cout << "File riprocessato con successo: " << fullPath << 
                          " (" << matchingPatterns.size() << " pattern/s applicati)" << std::endl;
            } else {
                WriteToLog("ERRORE: Nessun pattern corrisponde al file: " + filename + " nella cartella: " + folderPath);
                std::cerr << "ERRORE: Nessun pattern corrisponde al file nella cartella specificata. Controlla la configurazione." << std::endl;
            }
        } else {
            WriteToLog("ERRORE: File non trovato per il riprocessamento: " + fullPath);
            std::cerr << "ERRORE: File non trovato. Verifica che il file esista in " << folderPath << std::endl;
        }
    }
    // Opzione per creare/aggiornare la configurazione
    else if (argc > 1 && strcmp(argv[1], "config") == 0 && argc == 2) {
        WriteToLog("Creazione/aggiornamento configurazione multi-cartella");
        
        // Carica la configurazione esistente o crea quella di default
        LoadConfiguration();
        
        std::cout << "Configurazione multi-cartella attuale:" << std::endl;
        std::cout << "  File di configurazione: " << configFile << std::endl;
        std::cout << "  Cartella di default: " << defaultMonitoredFolder << std::endl;
        
        // Raggruppa i pattern per cartella
        std::map<std::string, std::vector<int>> folderPatterns;
        for (size_t i = 0; i < patternCommandPairs.size(); ++i) {
            folderPatterns[patternCommandPairs[i].folderPath].push_back(static_cast<int>(i));
        }
        
        std::cout << "  Cartelle monitorate: " << folderPatterns.size() << std::endl;
        for (const auto& folderGroup : folderPatterns) {
            std::cout << "    " << folderGroup.first << " (" << folderGroup.second.size() << " pattern/s)" << std::endl;
        }
        
        std::cout << "\nConfigurazione multi-cartella creata/aggiornata con successo." << std::endl;
    }
    // Altrimenti, avvia il servizio
    else {
        WriteToLog("Avvio servizio Windows multi-cartella");
        
        SERVICE_TABLE_ENTRY serviceTable[] = {
            { const_cast<LPSTR>(SERVICE_NAME), (LPSERVICE_MAIN_FUNCTION)ServiceMain },
            { NULL, NULL }
        };
        
        if (StartServiceCtrlDispatcher(serviceTable) == FALSE) {
            // Se StartServiceCtrlDispatcher fallisce, probabilmente il programma è stato eseguito direttamente
            DWORD err = GetLastError();
            WriteToLog("StartServiceCtrlDispatcher failed: " + std::to_string(err));
            std::cerr << "Errore: questo programma deve essere eseguito come servizio Windows." << std::endl;
            std::cerr << "Comandi disponibili per PatternTriggerCommand Multi-Folder:" << std::endl;
            std::cerr << "  PatternTriggerCommand.exe install     - installa il servizio" << std::endl;
            std::cerr << "  PatternTriggerCommand.exe uninstall   - disinstalla il servizio" << std::endl;
            std::cerr << "  PatternTriggerCommand.exe test        - testa in modalità console" << std::endl;
            std::cerr << "  PatternTriggerCommand.exe status      - visualizza stato del servizio" << std::endl;
            std::cerr << "  PatternTriggerCommand.exe reset       - resetta database file processati" << std::endl;
            std::cerr << "  PatternTriggerCommand.exe reprocess <cartella> <file> - riprocessa file specifico" << std::endl;
            std::cerr << "  PatternTriggerCommand.exe config      - crea/aggiorna configurazione" << std::endl;
            std::cerr << "  PatternTriggerCommand.exe config <percorso> - usa configurazione alternativa" << std::endl;
            return 1;
        }
    }
    
    WriteToLog("Applicazione multi-cartella terminata normalmente");
    return 0;
}
