CC = gcc
TARGET = paxsy
INSTALL_PATH = /usr/local/bin

# Version information
GENERATION = "beta 4"
NAME = "Rowan"
VERSION = "v0.4.1_8a"
DATE = "2026FEB08"

# Compiler flags with version definitions
CFLAGS = -std=c99 \
         -DGENERATION=\"$(GENERATION)\" \
         -DNAME=\"$(NAME)\" \
         -DVERSION=\"$(VERSION)\" \
         -DDATE=\"$(DATE)\" \

SRC = src/preprocessor/preprocessor.c \
      src/preprocessor/DPP__include/DPPF__include.c \
      src/lexer/lexer.c \
      src/parser/literals.c \
      src/parser/parser.c \
      src/semantic/semantic.c \
      src/output/output.c \
      src/errhandler/errhandler.c \
      src/main.c

all: build install

build: $(SRC)
	$(CC) $(CFLAGS) $^ -o $(TARGET)

install: $(TARGET)
	@if [ -w $(INSTALL_PATH) ]; then \
		cp $(TARGET) $(INSTALL_PATH); \
		echo "Installation is complete"; \
	else \
		echo "Root permissions are required to install from the $(INSTALL_PATH)"; \
		sudo cp $(TARGET) $(INSTALL_PATH); \
	fi

uninstall:
	@if [ -w $(INSTALL_PATH) ] && [ -f $(INSTALL_PATH)/$(TARGET) ]; then \
		rm $(INSTALL_PATH)/$(TARGET); \
		echo "Uninstallation is complete."; \
	else \
		echo "Root permissions are required to install from the $(INSTALL_PATH)"; \
		sudo rm -f $(INSTALL_PATH)/$(TARGET); \
	fi

clean:
	rm -f $(TARGET)

.PHONY: all build install uninstall clean
