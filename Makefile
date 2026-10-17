# =============================================================================
# Makefile - Dynamic Memory Allocation Tracker & Leak Detector
# Compatible with Ubuntu Linux / WSL
# =============================================================================

CC ?= gcc
AR ?= ar
RM := rm -rf

# Compilation Flags
STD_FLAGS     := -std=c11
WARN_FLAGS    := -Wall -Wextra -Wpedantic -Wconversion
OPT_FLAGS     := -g -O2
INC_FLAGS     := -Iinclude
THREAD_FLAGS  := -pthread

# Sanitizer Flags (Used with 'make asan' or 'make SANITIZER=1')
ASAN_FLAGS    := -fsanitize=address,undefined -fno-omit-frame-pointer

CFLAGS        ?= $(STD_FLAGS) $(WARN_FLAGS) $(OPT_FLAGS) $(INC_FLAGS) $(THREAD_FLAGS)
LDFLAGS       ?= $(THREAD_FLAGS)
ARFLAGS       := rcs

# Check if SANITIZER option is enabled
ifeq ($(strip $(SANITIZER)), 1)
    CFLAGS  += $(ASAN_FLAGS)
    LDFLAGS += $(ASAN_FLAGS)
endif

# Directories
SRC_DIR       := src
INC_DIR       := include
TEST_DIR      := tests
OBJ_DIR       := obj
BIN_DIR       := bin
LIB_DIR       := lib

# Targets
LIB_STATIC    := $(LIB_DIR)/libmemtrack.a
DEMO_BIN      := $(BIN_DIR)/memory_tracker_demo
TEST_BIN      := $(BIN_DIR)/test_memory_tracker

# Source & Object Files
TRACKER_SRC   := $(SRC_DIR)/memory_tracker.c
MAIN_SRC      := $(SRC_DIR)/main.c
TEST_SRC      := $(TEST_DIR)/test_memory_tracker.c

TRACKER_OBJ   := $(OBJ_DIR)/memory_tracker.o
MAIN_OBJ      := $(OBJ_DIR)/main.o
TEST_OBJ      := $(OBJ_DIR)/test_memory_tracker.o

# Default Target
.PHONY: all
all: directories $(LIB_STATIC) $(DEMO_BIN) $(TEST_BIN)
	@echo "Build complete. Executables ready in $(BIN_DIR)/"

# Ensure output directories exist
.PHONY: directories
directories:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)

# Static Library
$(LIB_STATIC): $(TRACKER_OBJ)
	@echo "Creating static library $(LIB_STATIC)..."
	$(AR) $(ARFLAGS) $@ $^

# Object Compilation Rules
$(OBJ_DIR)/memory_tracker.o: $(TRACKER_SRC) $(INC_DIR)/memory_tracker.h | directories
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/main.o: $(MAIN_SRC) $(INC_DIR)/memory_tracker.h | directories
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/test_memory_tracker.o: $(TEST_SRC) $(INC_DIR)/memory_tracker.h | directories
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Executables
$(DEMO_BIN): $(MAIN_OBJ) $(LIB_STATIC) | directories
	@echo "Linking demonstration binary $(DEMO_BIN)..."
	$(CC) $(MAIN_OBJ) $(LIB_STATIC) $(LDFLAGS) -o $@

$(TEST_BIN): $(TEST_OBJ) $(LIB_STATIC) | directories
	@echo "Linking test binary $(TEST_BIN)..."
	$(CC) $(TEST_OBJ) $(LIB_STATIC) $(LDFLAGS) -o $@

# Run Targets
.PHONY: run
run: $(DEMO_BIN)
	@echo "Running Demonstration Suite..."
	@$(DEMO_BIN)

.PHONY: test
test: $(TEST_BIN)
	@echo "Executing Automated Test Suite..."
	@$(TEST_BIN)

# Build with AddressSanitizer and UndefinedBehaviorSanitizer
.PHONY: asan
asan: clean
	@echo "Building with AddressSanitizer and UndefinedBehaviorSanitizer..."
	@$(MAKE) SANITIZER=1 all
	@echo "ASan build complete. Note: Intentional leak tests will trigger ASan leak reports."

# Clean Target
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	$(RM) $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)
	@echo "Clean complete."

# Help Target
.PHONY: help
help:
	@echo "Dynamic Memory Allocation Tracker & Leak Detector"
	@echo "Available Make Targets:"
	@echo "  make           - Build static library, demo executable, and test suite"
	@echo "  make test      - Build and run the automated test suite"
	@echo "  make run       - Build and execute standard demonstration program"
	@echo "  make asan      - Rebuild everything with AddressSanitizer enabled"
	@echo "  make clean     - Remove all build directories and binaries"
	@echo "  make help      - Display this help message"
