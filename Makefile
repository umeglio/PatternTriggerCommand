# Makefile per PatternTriggerCommand Multi-Folder
# Autore: Umberto Meglio - Supporto alla creazione: Claude di Anthropic

# Compilatore e flag
CXX = g++
CXXFLAGS = -Wall -std=c++11 -O2
LDFLAGS = -static

# Librerie necessarie
LDLIBS = -ladvapi32

TARGET = PatternTriggerCommand.exe
SRC = PatternTriggerCommand.cpp

# Target principale
all: $(TARGET)

# Compilazione
$(TARGET): $(SRC)
	@echo "Compilazione PatternTriggerCommand Multi-Folder..."
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)
	@echo "Compilazione completata: $(TARGET)"

# Pulizia
clean:
	@echo "Pulizia file compilati..."
	-del $(TARGET) *.o 2>nul
	@echo "Pulizia completata."

# Installazione servizio
install: $(TARGET)
	@echo "Installazione del servizio multi-cartella..."
	$(TARGET) install
	@echo "Servizio installato. Configurare C:\PTC\config.ini e avviare da Servizi Windows."

# Test in modalità console
test: $(TARGET)
	@echo "Avvio test in modalità console multi-cartella..."
	$(TARGET) test

# Verifica stato
status: $(TARGET)
	@echo "Verifica stato del servizio..."
	$(TARGET) status

# Reset database
reset: $(TARGET)
	@echo "Reset del database dei file processati..."
	$(TARGET) reset

# Disinstallazione
uninstall: $(TARGET)
	@echo "Disinstallazione del servizio..."
	$(TARGET) uninstall

# Configurazione
config: $(TARGET)
	@echo "Creazione/aggiornamento configurazione multi-cartella..."
	$(TARGET) config

# Target per debug
debug: CXXFLAGS += -g -DDEBUG
debug: $(TARGET)
	@echo "Versione debug compilata."

# Target per release ottimizzata
release: CXXFLAGS += -O3 -DNDEBUG
release: clean $(TARGET)
	@echo "Versione release ottimizzata compilata."

# Help
help:
	@echo "PatternTriggerCommand Multi-Folder - Targets disponibili:"
	@echo "  all       - Compila il progetto (default)"
	@echo "  clean     - Rimuove file compilati"
	@echo "  install   - Compila e installa il servizio"
	@echo "  test      - Compila e avvia in modalità test"
	@echo "  status    - Verifica stato servizio e configurazione"
	@echo "  reset     - Reset database file processati"
	@echo "  uninstall - Disinstalla il servizio"
	@echo "  config    - Crea/aggiorna configurazione"
	@echo "  debug     - Compila versione debug"
	@echo "  release   - Compila versione release ottimizzata"
	@echo "  help      - Mostra questo messaggio"

# Verifica requisiti
check:
	@echo "Verifica requisiti sistema..."
	@echo "Compilatore: $(CXX)"
	@$(CXX) --version
	@echo "Standard C++: $(CXXFLAGS)"
	@echo "Directory corrente: $(CURDIR)"
	@if not exist "C:\PTC" mkdir "C:\PTC"
	@echo "Directory C:\PTC verificata/creata."
	@if not exist "C:\Scripts" mkdir "C:\Scripts"
	@echo "Directory C:\Scripts verificata/creata."

# Setup completo ambiente
setup: check config
	@echo "Setup ambiente completato."
	@echo "Prossimi passi:"
	@echo "1. Modificare C:\PTC\config.ini con i tuoi pattern"
	@echo "2. Creare script in C:\Scripts"
	@echo "3. Eseguire 'mingw32-make install' per installare il servizio"

# Target per deployment
deploy: release install
	@echo "Deployment completato."

# Target di test rapido
quicktest: $(TARGET)
	@echo "Test rapido configurazione..."
	$(TARGET) status
	@echo "Per test completo usare: mingw32-make test"

.PHONY: all clean install test status reset uninstall config debug release help check setup deploy quicktest
