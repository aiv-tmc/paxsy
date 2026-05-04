CC = gcc
TARGET = paxsy
INSTALL_PATH ?= /usr/bin
LIB_SOURCE_PATH = ./lib
SRCDIR = src

# Version information
GENERATION = "beta 4"
NAME = "Rowan"
VERSION = "0.4.3a"
DATE = "2026APR27"

# Determine architecture and OS for library path
ARCH := $(shell uname -m)
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
    OS_SUFFIX := gnu_linux
else ifeq ($(UNAME_S), Darwin)
    OS_SUFFIX := darwin
else ifeq ($(findstring MINGW32, $(UNAME_S)), MINGW32)
    OS_SUFFIX := mingw32
else ifeq ($(findstring MINGW64, $(UNAME_S)), MINGW64)
    OS_SUFFIX := mingw64
else ifeq ($(UNAME_S), FreeBSD)
    OS_SUFFIX := freebsd
else
    OS_SUFFIX := $(shell echo $(UNAME_S) | tr '[:upper:]' '[:lower:]')
endif

# Base library installation directory (can be overridden)
LIB_BASE ?= /usr
SYS_ARCH := $(ARCH)-$(OS_SUFFIX)

# Library directories that will be embedded into compiler
PAXSY_LIBRARY_DIR = $(LIB_BASE)/include/paxsy
PAXSY_INCLUDE_DIR = $(LIB_BASE)/lib/paxsy/include

# Compilation flags with version and library path definitions
CFLAGS = -std=c11 \
         -DGENERATION=\"$(GENERATION)\" \
         -DNAME=\"$(NAME)\" \
         -DVERSION=\"v$(VERSION)\" \
         -DDATE=\"$(DATE)\" \
         -DPAXSY_LIBRARY_DIR=\"$(PAXSY_LIBRARY_DIR)\" \
         -DPAXSY_INCLUDE_DIR=\"$(PAXSY_INCLUDE_DIR)\"

# Source files
SRC := $(shell find $(SRCDIR) -type f -name '*.c')

INSTALL_LIBS ?= ask
SHELL := /bin/bash

# Default target
all: build install

# Compile the executable
build: $(SRC)
	$(CC) $(CFLAGS) $^ -o $(TARGET)
	@echo "Build completed: $(TARGET)"

# Install the executable and optionally libraries
install: build
	@echo ":: Installing executable to $(INSTALL_PATH)..."
	@if [ -w "$(INSTALL_PATH)" ]; then \
		cp $(TARGET) $(INSTALL_PATH)/; \
	else \
		echo "Superuser privileges required for installation to $(INSTALL_PATH)"; \
		sudo cp $(TARGET) $(INSTALL_PATH)/; \
	fi
	@echo "Executable installation completed."
	@echo ""
ifeq ($(INSTALL_LIBS), ask)
	@printf ":: Do you want to install libraries? [Y/n] "; \
	read answer; \
	if [[ "$$answer" =~ ^[Nn] ]]; then \
		echo "Skipping library installation."; \
	else \
		$(MAKE) install-libs; \
	fi
else ifeq ($(INSTALL_LIBS), yes)
	@$(MAKE) install-libs
else
	@echo "Skipping library installation (INSTALL_LIBS=$(INSTALL_LIBS))."
endif

# Install libraries (shared files and public includes)
install-libs:
	@# Install shared files (internal) to arch-specific path
	@if [ -d "$(LIB_SOURCE_PATH)/shared" ]; then \
		echo ":: Installing shared files to $(PAXSY_LIBRARY_DIR)..."; \
		if [ -w "$(PAXSY_LIBRARY_DIR)" ] 2>/dev/null || mkdir -p "$(PAXSY_LIBRARY_DIR)" 2>/dev/null; then \
			mkdir -p "$(PAXSY_LIBRARY_DIR)"; \
			cp -r $(LIB_SOURCE_PATH)/shared/* "$(PAXSY_LIBRARY_DIR)"/ 2>/dev/null || true; \
		else \
			echo "Superuser privileges required for shared installation"; \
			sudo mkdir -p "$(PAXSY_LIBRARY_DIR)"; \
			sudo cp -r $(LIB_SOURCE_PATH)/shared/* "$(PAXSY_LIBRARY_DIR)"/ 2>/dev/null || true; \
		fi; \
		echo "Shared files installed."; \
	else \
		echo "!! Directory $(LIB_SOURCE_PATH)/shared not found, skipping shared installation."; \
	fi
	@# Install public include files to versioned system include path
	@if [ -d "$(LIB_SOURCE_PATH)/include" ]; then \
		echo ":: Installing public include files to $(PAXSY_INCLUDE_DIR)..."; \
		if [ -w "$(PAXSY_INCLUDE_DIR)" ] 2>/dev/null || mkdir -p "$(PAXSY_INCLUDE_DIR)" 2>/dev/null; then \
			mkdir -p "$(PAXSY_INCLUDE_DIR)"; \
			cp -r $(LIB_SOURCE_PATH)/include/* "$(PAXSY_INCLUDE_DIR)"/ 2>/dev/null || true; \
		else \
			echo "Superuser privileges required for include installation"; \
			sudo mkdir -p "$(PAXSY_INCLUDE_DIR)"; \
			sudo cp -r $(LIB_SOURCE_PATH)/include/* "$(PAXSY_INCLUDE_DIR)"/ 2>/dev/null || true; \
		fi; \
		echo "Public include files installed."; \
	else \
		echo "!! Directory $(LIB_SOURCE_PATH)/include not found, skipping public include installation."; \
	fi

# Uninstall the executable
uninstall:
	@echo ":: Removing executable from $(INSTALL_PATH)..."
	@if [ -f "$(INSTALL_PATH)/$(TARGET)" ]; then \
		if [ -w "$(INSTALL_PATH)" ]; then \
			rm -f "$(INSTALL_PATH)/$(TARGET)"; \
		else \
			sudo rm -f "$(INSTALL_PATH)/$(TARGET)"; \
		fi; \
		echo "Executable uninstalled."; \
	else \
		echo "File $(INSTALL_PATH)/$(TARGET) not found."; \
	fi

# Uninstall libraries
uninstall-libs:
	@# Remove shared files
	@if [ -d "$(PAXSY_LIBRARY_DIR)" ]; then \
		echo ":: Removing shared files from $(PAXSY_LIBRARY_DIR)..."; \
		if [ -w "$(PAXSY_LIBRARY_DIR)" ]; then \
			rm -rf "$(PAXSY_LIBRARY_DIR)"; \
		else \
			sudo rm -rf "$(PAXSY_LIBRARY_DIR)"; \
		fi; \
		echo "Shared files removed."; \
	else \
		echo "Directory $(PAXSY_LIBRARY_DIR) not found, skipping."; \
	fi
	@# Remove public include files
	@if [ -d "$(PAXSY_INCLUDE_DIR)" ]; then \
		echo ":: Removing public include files from $(PAXSY_INCLUDE_DIR)..."; \
		if [ -w "$(PAXSY_INCLUDE_DIR)" ]; then \
			rm -rf "$(PAXSY_INCLUDE_DIR)"; \
		else \
			sudo rm -rf "$(PAXSY_INCLUDE_DIR)"; \
		fi; \
		echo "Public include files removed."; \
	else \
		echo "Directory $(PAXSY_INCLUDE_DIR) not found, skipping."; \
	fi

# Clean everything (remove installed files and binary)
clean: uninstall uninstall-libs
	@if [ -f $(TARGET) ]; then rm -f $(TARGET); fi
	@echo "Clean completed."

# Helper targets
print-info:
	@echo "Target: $(TARGET)"
	@echo "Install path: $(INSTALL_PATH)"
	@echo "Shared library path: $(PAXSY_LIBRARY_DIR)/incl"
	@echo "Public include path: $(PAXSY_INCLUDE_DIR)"
	@echo "Source libs: $(LIB_SOURCE_PATH)"
	@echo "Detected architecture: $(SYS_ARCH)"
	@echo "Version (clean): $(VERSION)"
	@echo "OS detected: $(UNAME_S) -> $(OS_SUFFIX)"
	@echo "Library base: $(LIB_BASE)"

.PHONY: all build install install-libs uninstall uninstall-libs clean print-info
