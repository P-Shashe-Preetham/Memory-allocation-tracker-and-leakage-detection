# Makefile for MemWatch — Linux Memory Monitor and Leak Detector in C

CC          ?= gcc
STD_FLAGS   := -std=c11 -Wall -Wextra -Wpedantic
OPT_FLAGS   ?= -O2
INCLUDES    := -Iinclude

# Resolve libxml2 flags via pkg-config, falling back to xml2-config or direct libxml2.so.2
XML2_CFLAGS := $(shell pkg-config --cflags libxml-2.0 2>/dev/null || xml2-config --cflags 2>/dev/null || echo "-Iinclude/compat")
XML2_LIBS   := $(shell pkg-config --libs libxml-2.0 2>/dev/null || xml2-config --libs 2>/dev/null || echo "-l:libxml2.so.2")

CFLAGS      := $(STD_FLAGS) $(OPT_FLAGS) $(INCLUDES) $(XML2_CFLAGS)
LDFLAGS     := $(XML2_LIBS) -lm

SAN_FLAGS   := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

PREFIX      ?= /usr/local
BINDIR      := $(PREFIX)/bin

TARGET      := memwatch
BUILD_DIR   := build

LIB_SRCS    := src/cli.c \
               src/proc_reader.c \
               src/system_memory.c \
               src/monitor.c \
               src/leak_analyzer.c \
               src/valgrind_parser.c \
               src/report.c \
               src/utils.c

MAIN_SRC    := src/main.c
LIB_OBJS    := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))
MAIN_OBJ    := $(BUILD_DIR)/main.o

TEST_BINS   := $(BUILD_DIR)/test_proc_reader \
               $(BUILD_DIR)/test_memory_calculations \
               $(BUILD_DIR)/test_growth_detection \
               $(BUILD_DIR)/test_report_parser

HELPER_BINS := tests/leak_target tests/mock_valgrind

.PHONY: all test clean install debug sanitize

all: $(TARGET) tests/leak_target

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(MAIN_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

tests/leak_target: tests/leak_target.c
	$(CC) $(STD_FLAGS) -g -O0 $< -o $@

tests/mock_valgrind: tests/mock_valgrind.c
	$(CC) $(STD_FLAGS) -g -O0 $< -o $@

$(BUILD_DIR)/test_%: tests/test_%.c $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -g $< $(LIB_OBJS) -o $@ $(LDFLAGS)

test: $(TEST_BINS) $(HELPER_BINS)
	@echo "=== Running MemWatch Unit Test Suite ==="
	./$(BUILD_DIR)/test_proc_reader
	./$(BUILD_DIR)/test_memory_calculations
	./$(BUILD_DIR)/test_growth_detection
	./$(BUILD_DIR)/test_report_parser
	@echo "=== All unit tests passed ==="

debug: OPT_FLAGS := -g -O0 -DDEBUG
debug: clean all

sanitize: $(HELPER_BINS) | $(BUILD_DIR)
	@echo "=== Building & Running Sanitized Tests (ASan + UBSan) ==="
	$(CC) $(STD_FLAGS) $(SAN_FLAGS) $(INCLUDES) $(XML2_CFLAGS) tests/test_proc_reader.c $(LIB_SRCS) -o $(BUILD_DIR)/san_test_proc_reader $(LDFLAGS)
	$(CC) $(STD_FLAGS) $(SAN_FLAGS) $(INCLUDES) $(XML2_CFLAGS) tests/test_memory_calculations.c $(LIB_SRCS) -o $(BUILD_DIR)/san_test_memory_calculations $(LDFLAGS)
	$(CC) $(STD_FLAGS) $(SAN_FLAGS) $(INCLUDES) $(XML2_CFLAGS) tests/test_growth_detection.c $(LIB_SRCS) -o $(BUILD_DIR)/san_test_growth_detection $(LDFLAGS)
	$(CC) $(STD_FLAGS) $(SAN_FLAGS) $(INCLUDES) $(XML2_CFLAGS) tests/test_report_parser.c $(LIB_SRCS) -o $(BUILD_DIR)/san_test_report_parser $(LDFLAGS)
	./$(BUILD_DIR)/san_test_proc_reader
	./$(BUILD_DIR)/san_test_memory_calculations
	./$(BUILD_DIR)/san_test_growth_detection
	./$(BUILD_DIR)/san_test_report_parser
	@echo "=== All sanitized tests passed with zero memory/UB errors ==="

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(BUILD_DIR)/* $(TARGET) $(HELPER_BINS)
	rmdir $(BUILD_DIR) 2>/dev/null || true
	rm -f reports/valgrind_*.xml reports/memwatch_report_*.txt reports/latest_report.txt
