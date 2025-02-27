# Makefile per PatternTriggerCommand

# Compilatore e flag
CXX = g++
CXXFLAGS = -Wall -std=c++11
LDFLAGS = -static

# Librerie necessarie
LDLIBS = -ladvapi32

TARGET = PatternTriggerCommand.exe
SRC = PatternTriggerCommand.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	del $(TARGET) *.o

install: $(TARGET)
	@echo "Installazione del servizio..."
	$(TARGET) install

test: $(TARGET)
	@echo "Avvio test in modalità console..."
	$(TARGET) test

status: $(TARGET)
	@echo "Verifica stato del servizio..."
	$(TARGET) status

reset: $(TARGET)
	@echo "Reset del database dei file processati..."
	$(TARGET) reset

uninstall: $(TARGET)
	@echo "Disinstallazione del servizio..."
	$(TARGET) uninstall

config: $(TARGET)
	@echo "Creazione/aggiornamento configurazione..."
	$(TARGET) config

.PHONY: all clean install test status reset uninstall config