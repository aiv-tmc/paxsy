CC = gcc
TARGET = paxsy
INSTALL_PATH = /bin
LIB_SOURCE_PATH = ./lib
SRCDIR = src

# Version information
GENERATION = "beta 4"
NAME = "Rowan"
VERSION = "0.4.1_8a"
DATE = "2026FEB08"

# Compilation flags with version definitions
CFLAGS = -std=c99 \
         -DGENERATION=\"$(GENERATION)\" \
         -DNAME=\"$(NAME)\" \
         -DVERSION=\"v$(VERSION)\" \
         -DDATE=\"$(DATE)\"

# Source files
SRC = src/preprocessor/preprocessor.c \
      src/preprocessor/defmacros/defmacros.c \
      src/preprocessor/directive/include/include.c \
      src/preprocessor/directive/define/macro.c \
      src/preprocessor/directive/define/define.c \
      src/preprocessor/directive/conditional/conditional.c \
      src/lexer/lexer.c \
      src/parser/literals.c \
      src/parser/parser.c \
      src/semantic/semantic.c \
      src/output/output.c \
      src/errhandler/errhandler.c \
      src/main.c \
      src/utils/str_utils.c \
      src/utils/char_utils.c \
      src/utils/memory_utils.c

# Determine architecture and OS
ARCH := $(shell uname -m)
UNAME_S := $(shell uname -s)

# Convert OS to a short identifier
ifeq ($(UNAME_S), Linux)
    OS_SUFFIX := gnu_linux
else ifeq ($(UNAME_S), Darwin)
    OS_SUFFIX := darwin
else ifeq ($(findstring MINGW32, $(UNAME_S)), MINGW32)
    OS_SUFFIX := mingw32
else ifeq ($(findstring MINGW64, $(UNAME_S)), MINGW64)
    OS_SUFFIX := mingw64
else ifeq ($(findstring CYGWIN, $(UNAME_S)), CYGWIN)
    OS_SUFFIX := cygwin
else ifeq ($(UNAME_S), FreeBSD)
    OS_SUFFIX := freebsd
else
    OS_SUFFIX := $(shell echo $(UNAME_S) | tr '[:upper:]' '[:lower:]')
endif

SYS_ARCH := $(ARCH)-$(OS_SUFFIX)
LIB_HEADER_PATH := /usr/lib/paxsy/$(SYS_ARCH)/incl
LIB_INCLUDE_PATH := /usr/include/paxsy

# Default target
all: build install

# Compile the executable
build: $(SRC)
	$(CC) $(CFLAGS) $^ -o $(TARGET)
	@echo "Build completed: $(TARGET)"

# Install the executable and optionally libraries
install: build
	@echo ":: Installing executable to $(INSTALL_PATH)..."
	@if [ -w $(INSTALL_PATH) ]; then \
		cp $(TARGET) $(INSTALL_PATH)/; \
	else \
		echo "Superuser privileges required for installation to $(INSTALL_PATH)"; \
		sudo cp $(TARGET) $(INSTALL_PATH)/; \
	fi
	@echo "Executable installation completed."
	@echo ""
	@printf ":: Do you want to install libraries? [Y/n] "; \
	read answer; \
	if [ "$$answer" = "n" ] || [ "$$answer" = "N" ]; then \
		echo "Skipping library installation."; \
	else \
		$(MAKE) install-libs; \
	fi

# Install libraries (header files and public includes)
install-libs:
	@# Install header files (internal) to arch-specific path
	@if [ -d "$(LIB_SOURCE_PATH)/header" ]; then \
		echo ":: Installing header files to $(LIB_HEADER_PATH)..."; \
		if [ -w $(LIB_HEADER_PATH) ] 2>/dev/null; then \
			mkdir -p $(LIB_HEADER_PATH); \
			cp -r $(LIB_SOURCE_PATH)/header/* $(LIB_HEADER_PATH)/ 2>/dev/null || true; \
		else \
			echo "Superuser privileges required for header installation"; \
			sudo mkdir -p $(LIB_HEADER_PATH); \
			sudo cp -r $(LIB_SOURCE_PATH)/header/* $(LIB_HEADER_PATH)/ 2>/dev/null || true; \
		fi; \
		echo "Header files installed."; \
	else \
		echo "!! Directory $(LIB_SOURCE_PATH)/header not found, skipping header installation."; \
	fi
	@# Install public include files to versioned system include path
	@if [ -d "$(LIB_SOURCE_PATH)/include" ]; then \
		echo ":: Installing public include files to $(LIB_INCLUDE_PATH)..."; \
		if [ -w $(LIB_INCLUDE_PATH) ] 2>/dev/null; then \
			mkdir -p $(LIB_INCLUDE_PATH); \
			cp -r $(LIB_SOURCE_PATH)/include/* $(LIB_INCLUDE_PATH)/ 2>/dev/null || true; \
		else \
			echo "Superuser privileges required for include installation"; \
			sudo mkdir -p $(LIB_INCLUDE_PATH); \
			sudo cp -r $(LIB_SOURCE_PATH)/include/* $(LIB_INCLUDE_PATH)/ 2>/dev/null || true; \
		fi; \
		echo "Public include files installed."; \
	else \
		echo "!! Directory $(LIB_SOURCE_PATH)/include not found, skipping public include installation."; \
	fi

# Uninstall the executable
uninstall:
	@echo ":: Removing executable from $(INSTALL_PATH)..."
	@if [ -f $(INSTALL_PATH)/$(TARGET) ]; then \
		if [ -w $(INSTALL_PATH) ]; then \
			rm -f $(INSTALL_PATH)/$(TARGET); \
		else \
			sudo rm -f $(INSTALL_PATH)/$(TARGET); \
		fi; \
		echo "Executable uninstalled."; \
	else \
		echo "File $(INSTALL_PATH)/$(TARGET) not found."; \
	fi

# Uninstall libraries
uninstall-libs:
	@# Remove header files
	@if [ -d "$(LIB_HEADER_PATH)" ]; then \
		echo ":: Removing header files from $(LIB_HEADER_PATH)..."; \
		if [ -w $(LIB_HEADER_PATH) ]; then \
			rm -rf $(LIB_HEADER_PATH); \
		else \
			sudo rm -rf $(LIB_HEADER_PATH); \
		fi; \
		echo "Header files removed."; \
	else \
		echo "Directory $(LIB_HEADER_PATH) not found, skipping."; \
	fi
	@# Remove public include files
	@if [ -d "$(LIB_INCLUDE_PATH)" ]; then \
		echo ":: Removing public include files from $(LIB_INCLUDE_PATH)..."; \
		if [ -w $(LIB_INCLUDE_PATH) ]; then \
			rm -rf $(LIB_INCLUDE_PATH); \
		else \
			sudo rm -rf $(LIB_INCLUDE_PATH); \
		fi; \
		echo "Public include files removed."; \
	else \
		echo "Directory $(LIB_INCLUDE_PATH) not found, skipping."; \
	fi

# Clean everything (remove installed files and local binary)
clean: uninstall uninstall-libs
	@if [ -f $(TARGET) ]; then rm -f $(TARGET); fi
	@echo "Clean completed."

# Helper targets
print-info:
	@echo "Target: $(TARGET)"
	@echo "Install path: $(INSTALL_PATH)"
	@echo "Header library path: $(LIB_HEADER_PATH)"
	@echo "Public include path: $(LIB_INCLUDE_PATH)"
	@echo "Source libs: $(LIB_SOURCE_PATH)"
	@echo "Detected architecture: $(SYS_ARCH)"
	@echo "Version (clean): $(VERSION)"

.PHONY: all build install install-libs uninstall uninstall-libs clean print-info
