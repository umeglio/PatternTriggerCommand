# PatternTriggerCommand Multi-Folder v3.1

> 🇬🇧 The main documentation is in English: **[README.md](README.md)**
>
> 🌐 Pagina del progetto: **[umeglio.github.io/PatternTriggerCommand](https://umeglio.github.io/PatternTriggerCommand/)**

[![C++11 nativo](https://img.shields.io/badge/nativo-C%2B%2B11-2fd0ff?style=flat-square&labelColor=03060f)](#architettura-tecnica)
[![API Win32](https://img.shields.io/badge/Win32-API%20%C2%B7%20Winsock2-2fd0ff?style=flat-square&labelColor=03060f)](#architettura-tecnica)
[![Zero dipendenze](https://img.shields.io/badge/dipendenze-zero-7dffb0?style=flat-square&labelColor=03060f)](#architettura-tecnica)
[![Build statica](https://img.shields.io/badge/build-MinGW%20statica-ffcf6b?style=flat-square&labelColor=03060f)](#compilazione)
[![Licenza MIT](https://img.shields.io/badge/licenza-MIT-7dffb0?style=flat-square&labelColor=03060f)](LICENSE)
[![Composto in miraFONT](https://img.shields.io/badge/composto%20in-miraFONT-c792ea?style=flat-square&labelColor=03060f)](https://github.com/umeglio/miraFONT)

**Autore:** Umberto Meglio
**Supporto alla creazione:** Claude di Anthropic
**Versione corrente:** 3.1
**Licenza:** [MIT](LICENSE)

![PatternTriggerCommand Dashboard](screenshot.png)

---

## Panoramica

PatternTriggerCommand e' un servizio Windows in C++ nativo che monitora cartelle multiple per file corrispondenti a pattern regex ed esegue automaticamente comandi associati. Include una dashboard web integrata per il monitoraggio in tempo reale e uno **schedulatore parametrico** per l'esecuzione pianificata di comandi e una **pagina impostazioni** che modifica la configurazione del servizio e la applica a caldo.

Tutto sta in un unico eseguibile a link statico: niente .NET, niente Java, nessuna libreria esterna. La dashboard e' composta in [miraFONT](https://github.com/umeglio/miraFONT), incorporato nel binario, quindi si vede identica anche su un server senza accesso a internet.

## Funzionalita' Principali

### Monitoraggio Multi-Cartella
- Monitoraggio nativo di cartelle diverse con configurazioni pattern specifiche
- Espressioni regolari per identificare i file di interesse (case-insensitive, corrispondenza sull'intero nome, alternanze con `|` ammesse se la cartella e' esplicita)
- Esecuzione automatica di comandi al rilevamento di file corrispondenti
- Database persistente dei file processati per evitare duplicati

### Schedulatore Parametrico
- **Trigger settimanale**: selezione giorni (Lu, Ma, Me, Gi, Ve, Sa, Do)
- **Trigger orario**: ore specifiche (es: 1,6,12,18)
- **Trigger al minuto**: minuti specifici (es: 0,15,30,45)
- **Modalita' intervallo**: ripeti ogni N secondi (minimo 5 sec)
- Configurazione su filesystem in file `.sch`
- Pagina web dedicata con CRUD completo
- Storico esecuzioni con filtri per nome e stato

### Dashboard Web
- Interfaccia responsive HTML5 con tema scuro "miraNET", lo stesso stile grafico del sito umeglio.it
- Tipografia interamente in **miraFONT** (Spline per i titoli, Linear per il testo, Dots per i numeri), il carattere originale di Umberto Meglio incorporato nell'eseguibile: nessuna dipendenza da internet
- Statistiche in tempo reale: file processati, comandi, errori, memoria, thread, uptime
- Grafici: andamento ultime 24 ore (file, comandi ed errori al minuto), distribuzione oraria di oggi, memoria del servizio, pattern piu' attivi, esito dello schedulatore
- Tabelle cartelle monitorate e pattern configurati, feed attivita' recente con orario reale
- Indicatore di connessione con il servizio e notifiche toast al posto dei popup
- Auto-aggiornamento ogni 2 secondi (grafici ogni 15 secondi)

### Pagina Impostazioni
- Modifica di tutta la configurazione del servizio direttamente dal browser: cartelle, file di log, database file processati, log dettagliato, schedulatore, porta e stato del web server
- Editor dei pattern a tabella (nome, cartella, regex, comando) con validazione della regex e prova su un nome file
- Salvataggio atomico su `config.ini` e **applicazione a caldo**: i monitor vengono riavviati senza fermare il servizio; se cambia la porta il web server si riavvia da solo sul nuovo indirizzo
- Pulsante "Ricarica da disco" per applicare modifiche fatte a mano al file

### Pagina Schedulatore
- Gestione task: crea, modifica, duplica, elimina
- Toggle attiva/disattiva per ogni task
- Selettore file: scansiona `C:\Scripts` per `.bat`, `.cmd`, `.exe`, `.ps1`
- Switch tra modalita' programmata (giorno/ora/minuto) e intervallo (ogni N secondi)
- Storico esecuzioni con filtro per nome task e stato (successo/errore)
- Chip interattivi per selezione giorni della settimana

### REST API
- `GET /` - Dashboard principale
- `GET /scheduler` - Pagina gestione schedulatore
- `GET /settings` - Pagina impostazioni del servizio
- `GET /api/metrics` - Metriche di sistema in JSON
- `GET /api/stats` - Serie storiche e statistiche per i grafici (campione ogni minuto, ultime 24 ore)
- `GET /api/config` - Configurazione corrente in JSON
- `POST /api/config/save` - Salva `config.ini` e applica la configurazione a caldo
- `POST /api/config/reload` - Rilegge `config.ini` dal disco e riavvia i monitor
- `GET /api/scheduler` - Task schedulati e storico in JSON
- `GET /api/scheduler/scripts` - Elenco script disponibili
- `POST /api/scheduler/save` - Salva/modifica task
- `POST /api/scheduler/delete` - Elimina task
- `POST /api/scheduler/toggle` - Attiva/disattiva task
- `GET /fonts/<nome>.woff` - Font miraFONT incorporati

## Quick Start

### Compilazione
```bash
# Con MinGW nel PATH
mingw32-make

# Oppure percorso completo
G:\mingw32\bin\mingw32-make.exe
```

Il repository include anche l'eseguibile statico a 32 bit gia' pronto, `PatternTriggerCommand.exe`.

### Installazione Servizio
```bash
PatternTriggerCommand.exe install
```

### Test in Console
```bash
PatternTriggerCommand.exe test
```
Apri `http://localhost:8080` per la dashboard, `http://localhost:8080/scheduler` per lo schedulatore e `http://localhost:8080/settings` per le impostazioni.

## Configurazione

Il servizio crea automaticamente il file di configurazione `C:\PTC\config.ini`:

```ini
[Settings]
DefaultMonitoredFolder=C:\Monitored
LogFile=C:\PTC\PatternTriggerCommand.log
DetailedLogFile=C:\PTC\PatternTriggerCommand_detailed.log
ProcessedFilesDB=C:\PTC\PatternTriggerCommand_processed.txt
DetailedLogging=true
WebServerPort=8080
WebServerEnabled=true
SchedulerEnabled=true
SchedulerFolder=C:\PTC\schedules

[Patterns]
# Formato esteso: Cartella|Pattern|Comando
Pattern1=C:\Invoices\Incoming|^invoice.*\.pdf$|C:\Scripts\process_invoice.bat
Pattern2=C:\Reports\Monthly|^[0-9]{8}_.*DEMAT.*\.csv$|C:\Scripts\process_demat.bat

# Formato legacy: Pattern|Comando (usa cartella default)
Pattern3=^backup.*\.zip$|C:\Scripts\process_backup.bat
```

### Configurazione Schedulatore

I task schedulati sono file `.sch` nella cartella `C:\PTC\schedules\`:

```ini
# Esempio: C:\PTC\schedules\Backup_giornaliero.sch
Name=Backup giornaliero
Enabled=true
Days=Lu,Ma,Me,Gi,Ve
Hours=9,17
Minutes=0,30
Command=C:\Scripts\backup.bat
Interval=0
```

**Modalita' intervallo** (ripeti ogni N secondi):
```ini
Name=Health check
Enabled=true
Command=C:\Scripts\health_check.bat
Interval=300
```

Quando `Interval` e' > 0, i campi Days/Hours/Minutes vengono ignorati e il task viene eseguito ogni N secondi.

## Interfaccia Web

### Dashboard (`http://localhost:8080`)

La dashboard mostra in tempo reale:

| Sezione | Contenuto |
|---------|-----------|
| Stat Cards | File processati, file oggi, comandi eseguiti, errori, memoria, thread, uptime, ultima attivita' |
| Monitoraggio | Cartelle monitorate, pattern configurati, stato web server e schedulatore |
| Attivita' Recente | Feed eventi con timestamp |
| Cartelle | Tabella con stato, percorso, file rilevati/processati |
| Pattern | Tabella con nome, cartella, regex, match, esecuzioni |

### Impostazioni (`http://localhost:8080/settings`)

| Sezione | Contenuto |
|---------|-----------|
| Percorsi | Cartella monitorata predefinita, cartella task, file di log, log dettagliato, database file processati |
| Servizio | Interruttori log dettagliato / schedulatore / web server, porta del web server |
| Pattern e comandi | Tabella modificabile dei pattern con prova regex su un nome file, aggiunta e rimozione righe |
| Barra di salvataggio | Indicatore modifiche non salvate, "Annulla modifiche", "Salva e applica" |

Il salvataggio riscrive `C:\PTC\config.ini` e chiede al servizio di ricaricarsi a caldo: i monitor delle cartelle vengono fermati e riavviati con i nuovi pattern, lo schedulatore rilegge la sua cartella e, se porta o stato del web server sono cambiati, il web server viene riavviato. Disattivando il web server dalla pagina la dashboard non sara' piu' raggiungibile: si riattiva da `config.ini`.

### Schedulatore (`http://localhost:8080/scheduler`)

| Sezione | Contenuto |
|---------|-----------|
| Stat Cards | Stato schedulatore, task totali, task attivi, esecuzioni totali |
| Tab Task | Tabella task con nome, stato, tipo (programmato/intervallo), programmazione, comando, azioni |
| Tab Storico | Log esecuzioni con data/ora, task, comando, esito, codice uscita |
| Form Modifica | Nome, modalita' (giorno-ora-minuto o intervallo), selezione giorni, ore, minuti, selettore file |

**Azioni disponibili per ogni task:**
- **Avvia/Stop** - attiva o disattiva il task
- **Mod** - apri form di modifica
- **Dup** - duplica il task con nome "(copia)"
- **Elim** - elimina il task (con conferma)

## Comandi CLI

```bash
PatternTriggerCommand.exe install              # Installa come servizio Windows
PatternTriggerCommand.exe uninstall            # Rimuovi servizio
PatternTriggerCommand.exe test                 # Modalita' console (CTRL+C per uscire)
PatternTriggerCommand.exe status               # Stato servizio e configurazione
PatternTriggerCommand.exe reset                # Reset database file processati
PatternTriggerCommand.exe config               # Crea/aggiorna configurazione
PatternTriggerCommand.exe reprocess <dir> <f>  # Riprocessa un file specifico
```

## Make Targets

```bash
mingw32-make            # Compila il progetto
mingw32-make debug      # Compila versione debug
mingw32-make release    # Compila versione release ottimizzata
mingw32-make fonts      # Rigenera miraFONT_embedded.h (Python 3)
mingw32-make install    # Compila e installa servizio
mingw32-make test       # Compila e avvia in console
mingw32-make status     # Verifica stato servizio
mingw32-make clean      # Pulisci file compilati
mingw32-make reset      # Reset database
mingw32-make setup      # Setup completo ambiente
mingw32-make deploy     # Deploy per produzione
```

## Esempi Pattern

### Documenti Aziendali
```ini
Pattern1=C:\Fatture|^(invoice|INV)_[0-9]{4}.*\.pdf$|C:\Scripts\process_invoice.bat
Pattern2=C:\Banca|^[0-9]{8}_.*DEMAT.*\.csv$|C:\Scripts\import_banking.bat
Pattern3=C:\Contratti|^contract_.*_signed\.pdf$|C:\Scripts\archive_contract.bat
```

### Operazioni IT
```ini
Pattern1=C:\Logs|^app_[0-9]{8}_[0-9]{6}\.log$|C:\Scripts\analyze_logs.bat
Pattern2=C:\Backups|^db_backup_.*\.sql\.gz$|C:\Scripts\verify_backup.bat
Pattern3=C:\Config|^config_v[0-9]+\.[0-9]+\.xml$|C:\Scripts\deploy_config.bat
```

## Web server

Il web server integrato serve ogni connessione in un thread dedicato, legge le richieste per intero (intestazioni e body secondo `Content-Length`) e invia le risposte fino all'ultimo byte: le pagine non arrivano piu' troncate e le richieste POST non vanno perse, difetti presenti nella versione 3.0 dove il socket accettato ereditava la modalita' non bloccante. Le regex nel file di configurazione possono contenere `|` (alternanze) se la cartella e' indicata esplicitamente.

## miraFONT

I file WOFF in `fonts/miraFONT/` (licenza CC0, da https://github.com/umeglio/miraFONT) vengono incorporati nell'eseguibile tramite l'header `miraFONT_embedded.h`, generato con:

```bash
python tools/embed_fonts.py      # oppure: mingw32-make fonts
```

## Pagina del progetto

La cartella `docs/` contiene la pagina bilingue del progetto pubblicata su https://umeglio.github.io/PatternTriggerCommand/ dal workflow `deploy-pages.yml` (GitHub Pages con sorgente "GitHub Actions"). La pagina sceglie italiano o inglese dalla lingua del browser e ricorda la scelta del visitatore.

## Architettura Tecnica

- **Linguaggio**: C++11 nativo con MinGW, API Win32, Winsock2, PSAPI
- **Piattaforma**: Windows 7+ / Server 2008 R2+
- **Thread**: Multi-thread con mutex per thread safety
- **Web Server**: HTTP/1.1 integrato su socket Windows, un thread per connessione
- **Monitoraggio**: `ReadDirectoryChangesW` asincrono per ogni cartella
- **Schedulatore**: Thread dedicato con check ogni 15 secondi (sleep frazionato per shutdown rapido)
- **Librerie**: advapi32, kernel32, user32, ws2_32, psapi (incluse in Windows)
- **Build**: Makefile con MinGW, linking statico per portabilita'

## Struttura Directory

```
C:\PTC\
  config.ini                           # Configurazione principale
  PatternTriggerCommand.log            # Log attivita'
  PatternTriggerCommand_detailed.log   # Log dettagliato
  PatternTriggerCommand_processed.txt  # Database file processati
  schedules\                           # Task schedulati
    Backup_giornaliero.sch
    Health_check.sch
    ...

C:\Scripts\                            # Script eseguibili
  process_invoice.bat
  backup.bat
  ...
```

## Requisiti di Sistema

- **OS**: Windows 7/Server 2008 R2 o superiore
- **Compilatore**: MinGW con supporto C++11
- **Permessi**: Amministratore per installazione servizio
- **Rete**: Porta configurabile (default 8080) per dashboard web

## Licenza

MIT License - Vedi file LICENSE per i dettagli.

---

*PatternTriggerCommand v3.1 - Monitoraggio multi-cartella con schedulatore parametrico e dashboard web integrata.*
