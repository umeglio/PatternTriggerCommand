# Makefile per PatternTriggerCommand Multi-Folder v2.0
# Autore: Umberto Meglio - Supporto: Claude di Anthropic

# Compilatore e flag ottimizzati
CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++11 -O2 -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
CXXFLAGS_DEBUG = -Wall -Wextra -std=c++11 -g -DDEBUG -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
LDFLAGS = -static -static-libgcc -static-libstdc++

# Librerie necessarie
LDLIBS = -ladvapi32 -lkernel32 -luser32 -lws2_32 -lpsapi

TARGET = PatternTriggerCommand.exe
SRC = PatternTriggerCommand.cpp

# Colori per output (se supportati)
COLOR_RESET = \033[0m
COLOR_GREEN = \033[32m
COLOR_YELLOW = \033[33m
COLOR_RED = \033[31m
COLOR_BLUE = \033[34m

# Target principale
all: $(TARGET)
	@echo "$(COLOR_GREEN)✓ Compilazione completata: $(TARGET)$(COLOR_RESET)"

# Compilazione versione release
$(TARGET): $(SRC)
	@echo "$(COLOR_BLUE)Compilazione PatternTriggerCommand Multi-Folder v2.0...$(COLOR_RESET)"
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

# Compilazione versione debug
debug: $(SRC)
	@echo "$(COLOR_YELLOW)Compilazione versione debug...$(COLOR_RESET)"
	$(CXX) $(CXXFLAGS_DEBUG) -o $(TARGET) $< $(LDFLAGS) $(LDLIBS)
	@echo "$(COLOR_GREEN)✓ Versione debug compilata$(COLOR_RESET)"

# Compilazione versione release ottimizzata
release: clean
	@echo "$(COLOR_BLUE)Compilazione versione release ottimizzata...$(COLOR_RESET)"
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -o $(TARGET) $(SRC) $(LDFLAGS) $(LDLIBS)
	@echo "$(COLOR_GREEN)✓ Versione release ottimizzata compilata$(COLOR_RESET)"

# Pulizia file compilati
clean:
	@echo "$(COLOR_YELLOW)Pulizia file compilati...$(COLOR_RESET)"
	-del $(TARGET) *.o *.exe.stackdump core 2>nul || true
	@echo "$(COLOR_GREEN)✓ Pulizia completata$(COLOR_RESET)"

# Installazione servizio
install: $(TARGET)
	@echo "$(COLOR_BLUE)Installazione del servizio multi-cartella...$(COLOR_RESET)"
	@$(TARGET) status 2>nul || echo "Servizio non ancora installato"
	$(TARGET) install
	@echo "$(COLOR_GREEN)✓ Servizio installato. Configurare C:\PTC\config.ini e avviare da Servizi Windows$(COLOR_RESET)"

# Test in modalità console
test: $(TARGET)
	@echo "$(COLOR_BLUE)Avvio test in modalità console multi-cartella...$(COLOR_RESET)"
	@echo "$(COLOR_YELLOW)Usare CTRL+C per terminare$(COLOR_RESET)"
	$(TARGET) test

# Verifica stato completo
status: $(TARGET)
	@echo "$(COLOR_BLUE)Verifica stato del servizio...$(COLOR_RESET)"
	$(TARGET) status

# Reset database file processati
reset: $(TARGET)
	@echo "$(COLOR_YELLOW)Reset del database dei file processati...$(COLOR_RESET)"
	$(TARGET) reset
	@echo "$(COLOR_GREEN)✓ Database reset completato$(COLOR_RESET)"

# Disinstallazione servizio
uninstall: $(TARGET)
	@echo "$(COLOR_RED)Disinstallazione del servizio...$(COLOR_RESET)"
	$(TARGET) uninstall
	@echo "$(COLOR_GREEN)✓ Servizio disinstallato$(COLOR_RESET)"

# Configurazione
config: $(TARGET)
	@echo "$(COLOR_BLUE)Creazione/aggiornamento configurazione multi-cartella...$(COLOR_RESET)"
	$(TARGET) config
	@echo "$(COLOR_GREEN)✓ Configurazione aggiornata$(COLOR_RESET)"

# Verifica requisiti sistema
check:
	@echo "$(COLOR_BLUE)Verifica requisiti sistema...$(COLOR_RESET)"
	@echo "Compilatore: $(CXX)"
	@$(CXX) --version 2>/dev/null || echo "$(COLOR_RED)ERRORE: Compilatore non trovato$(COLOR_RESET)"
	@echo "Standard C++: C++11"
	@echo "Directory corrente: $(CURDIR)"
	@if not exist "C:\PTC" mkdir "C:\PTC" 2>nul || echo "Directory C:\PTC esistente"
	@echo "$(COLOR_GREEN)✓ Directory C:\PTC verificata/creata$(COLOR_RESET)"
	@if not exist "C:\Scripts" mkdir "C:\Scripts" 2>nul || echo "Directory C:\Scripts esistente"
	@echo "$(COLOR_GREEN)✓ Directory C:\Scripts verificata/creata$(COLOR_RESET)"

# Setup completo ambiente
setup: check all config
	@echo "$(COLOR_GREEN)✓ Setup ambiente completato$(COLOR_RESET)"
	@echo ""
	@echo "$(COLOR_BLUE)Prossimi passi:$(COLOR_RESET)"
	@echo "1. Modificare C:\PTC\config.ini con i tuoi pattern"
	@echo "2. Creare script di elaborazione in C:\Scripts"
	@echo "3. Eseguire 'mingw32-make install' per installare il servizio"
	@echo "4. Avviare il servizio da Gestione Servizi Windows"

# Deploy completo per produzione
deploy: clean release install
	@echo "$(COLOR_GREEN)✓ Deployment completato$(COLOR_RESET)"
	@echo "$(COLOR_BLUE)Servizio installato e pronto per l'avvio$(COLOR_RESET)"

# Test rapido configurazione
quicktest: $(TARGET)
	@echo "$(COLOR_BLUE)Test rapido configurazione...$(COLOR_RESET)"
	$(TARGET) status
	@echo ""
	@echo "$(COLOR_YELLOW)Per test completo usare: mingw32-make test$(COLOR_RESET)"

# Ricompilazione forzata
rebuild: clean all
	@echo "$(COLOR_GREEN)✓ Ricompilazione completata$(COLOR_RESET)"

# Verifica memory leaks (se disponibile)
memcheck: debug
	@echo "$(COLOR_BLUE)Controllo memory leaks...$(COLOR_RESET)"
	@echo "$(COLOR_YELLOW)Avviare manualmente con valgrind se disponibile$(COLOR_RESET)"

# Backup configurazione
backup:
	@echo "$(COLOR_BLUE)Backup configurazione...$(COLOR_RESET)"
	@if exist "C:\PTC\config.ini" copy "C:\PTC\config.ini" "C:\PTC\config.ini.bak" >nul
	@if exist "C:\PTC\PatternTriggerCommand_processed.txt" copy "C:\PTC\PatternTriggerCommand_processed.txt" "C:\PTC\PatternTriggerCommand_processed.txt.bak" >nul
	@echo "$(COLOR_GREEN)✓ Backup completato$(COLOR_RESET)"

# Ripristino configurazione
restore:
	@echo "$(COLOR_YELLOW)Ripristino configurazione...$(COLOR_RESET)"
	@if exist "C:\PTC\config.ini.bak" copy "C:\PTC\config.ini.bak" "C:\PTC\config.ini" >nul
	@if exist "C:\PTC\PatternTriggerCommand_processed.txt.bak" copy "C:\PTC\PatternTriggerCommand_processed.txt.bak" "C:\PTC\PatternTriggerCommand_processed.txt" >nul
	@echo "$(COLOR_GREEN)✓ Ripristino completato$(COLOR_RESET)"

# Test pattern regex
test-pattern:
	@echo "$(COLOR_BLUE)Test pattern configurati...$(COLOR_RESET)"
	@$(TARGET) status 2>nul || echo "$(COLOR_RED)Servizio non configurato$(COLOR_RESET)"

# Visualizza log in tempo reale (richiede tail o equivalent)
logs:
	@echo "$(COLOR_BLUE)Monitoraggio log in tempo reale...$(COLOR_RESET)"
	@if exist "C:\PTC\PatternTriggerCommand.log" type "C:\PTC\PatternTriggerCommand.log"
	@echo "$(COLOR_YELLOW)Per monitoraggio continuo usare tail -f se disponibile$(COLOR_RESET)"

# Help esteso
help:
	@echo "$(COLOR_BLUE)PatternTriggerCommand Multi-Folder v2.0 - Targets disponibili:$(COLOR_RESET)"
	@echo ""
	@echo "$(COLOR_GREEN)Compilazione:$(COLOR_RESET)"
	@echo "  all         - Compila il progetto (default)"
	@echo "  debug       - Compila versione debug"
	@echo "  release     - Compila versione release ottimizzata"
	@echo "  rebuild     - Ricompilazione forzata"
	@echo "  clean       - Rimuove file compilati"
	@echo ""
	@echo "$(COLOR_GREEN)Gestione Servizio:$(COLOR_RESET)"
	@echo "  install     - Compila e installa il servizio"
	@echo "  uninstall   - Disinstalla il servizio"
	@echo "  status      - Verifica stato servizio e configurazione"
	@echo "  test        - Avvia in modalità console per test"
	@echo ""
	@echo "$(COLOR_GREEN)Configurazione:$(COLOR_RESET)"
	@echo "  config      - Crea/aggiorna configurazione"
	@echo "  reset       - Reset database file processati"
	@echo "  backup      - Backup configurazione"
	@echo "  restore     - Ripristina configurazione"
	@echo ""
	@echo "$(COLOR_GREEN)Setup e Deploy:$(COLOR_RESET)"
	@echo "  setup       - Setup completo ambiente"
	@echo "  deploy      - Deploy completo per produzione"
	@echo "  check       - Verifica requisiti sistema"
	@echo "  quicktest   - Test rapido configurazione"
	@echo ""
	@echo "$(COLOR_GREEN)Utilità:$(COLOR_RESET)"
	@echo "  logs        - Visualizza log"
	@echo "  memcheck    - Controllo memory leaks"
	@echo "  help        - Mostra questo messaggio"
	@echo ""
	@echo "$(COLOR_YELLOW)Esempi d'uso:$(COLOR_RESET)"
	@echo "  mingw32-make setup     # Setup iniziale completo"
	@echo "  mingw32-make test      # Test in modalità console"
	@echo "  mingw32-make deploy    # Deploy per produzione"

# Assicura che i target senza file siano sempre eseguiti
.PHONY: all clean install test status reset uninstall config debug release help check setup deploy quicktest rebuild memcheck backup restore test-pattern logs

# Target di default
.DEFAULT_GOAL := all
