CC ?= cc
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
UNAME_S := $(shell uname -s)

# Directories
SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
BUILD_DIR ?= build

BASE_CFLAGS = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L \
	-I$(INC_DIR) \
	-DBUILD_DATE='"$(BUILD_DATE)"' -DGIT_HASH='"$(GIT_HASH)"'
CFLAGS ?= $(BASE_CFLAGS)
LDFLAGS ?=
LDLIBS ?= -lcurl -lsqlite3 -ldl -lm

TARGET = dsco
DEBUG_TARGET = $(TARGET)-debug

SRC_NAMES = main.c agent.c llm.c tools.c json_util.c ast.c swarm.c tui.c \
	md.c baseline.c setup.c crypto.c eval.c pipeline.c plugin.c \
	semantic.c ipc.c mcp.c provider.c integrations.c error.c trace.c task_profile.c \
	output_guard.c topology.c workspace.c plan.c router.c
TEST_SRC_NAMES = test.c

SRCS = $(addprefix $(SRC_DIR)/, $(SRC_NAMES))
# Test links against all src objects except main.c and agent.c
LIB_SRCS = $(filter-out $(SRC_DIR)/main.c $(SRC_DIR)/agent.c, $(SRCS))

OBJ_DIR := $(BUILD_DIR)/obj
DEBUG_OBJ_DIR := $(BUILD_DIR)/obj-debug
TEST_OBJ_DIR := $(BUILD_DIR)/test
ASAN_OBJ_DIR := $(BUILD_DIR)/asan
UBSAN_OBJ_DIR := $(BUILD_DIR)/ubsan
ASAN_TEST_OBJ_DIR := $(BUILD_DIR)/asan-test
UBSAN_TEST_OBJ_DIR := $(BUILD_DIR)/ubsan-test

OBJS = $(SRC_NAMES:%.c=$(OBJ_DIR)/%.o)
LIB_OBJS = $(filter-out $(OBJ_DIR)/main.o $(OBJ_DIR)/agent.o, $(OBJS))
TEST_OBJS = $(TEST_SRC_NAMES:%.c=$(TEST_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%)
DEBUG_OBJS = $(SRC_NAMES:%.c=$(DEBUG_OBJ_DIR)/%.o)
ASAN_OBJS = $(SRC_NAMES:%.c=$(ASAN_OBJ_DIR)/%.o)
UBSAN_OBJS = $(SRC_NAMES:%.c=$(UBSAN_OBJ_DIR)/%.o)
ASAN_TEST_OBJS = $(TEST_SRC_NAMES:%.c=$(ASAN_TEST_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(ASAN_TEST_OBJ_DIR)/%)
UBSAN_TEST_OBJS = $(TEST_SRC_NAMES:%.c=$(UBSAN_TEST_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(UBSAN_TEST_OBJ_DIR)/%)

ASAN_CFLAGS = $(BASE_CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address
ASAN_LDFLAGS = -fsanitize=address
UBSAN_CFLAGS = $(BASE_CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=all
UBSAN_LDFLAGS = -fsanitize=undefined -fno-sanitize-recover=all
DEBUG_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -DDSCO_DEV_BINARY
ASAN_RUNTIME_OPTIONS = detect_leaks=1
ifeq ($(UNAME_S),Darwin)
ASAN_RUNTIME_OPTIONS = detect_leaks=0
endif

PREFIX ?= /usr/local
DSCO_DIR = $(HOME)/.dsco

# Detect readline
READLINE_CHECK := $(shell echo '\#include <readline/readline.h>' | $(CC) -E -x c - >/dev/null 2>&1 && echo yes)
ifeq ($(READLINE_CHECK),yes)
BASE_CFLAGS += -DHAVE_READLINE
LDLIBS += -lreadline
endif

all: $(TARGET)
debug: $(DEBUG_TARGET)
dev: $(DEBUG_TARGET)

$(DEBUG_TARGET): $(DEBUG_OBJS)
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Source compilation rules
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(DEBUG_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(DEBUG_OBJ_DIR)
	$(CC) $(DEBUG_CFLAGS) -c -o $@ $<

$(ASAN_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(ASAN_OBJ_DIR)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(UBSAN_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(UBSAN_OBJ_DIR)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

# Test compilation rules — test sources from tests/, lib sources from src/
$(TEST_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(TEST_OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(TEST_OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(ASAN_TEST_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(ASAN_TEST_OBJ_DIR)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(ASAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(ASAN_TEST_OBJ_DIR)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(UBSAN_TEST_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(UBSAN_TEST_OBJ_DIR)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

$(UBSAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(UBSAN_TEST_OBJ_DIR)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

$(OBJ_DIR) $(DEBUG_OBJ_DIR) $(TEST_OBJ_DIR) $(ASAN_OBJ_DIR) $(UBSAN_OBJ_DIR) $(ASAN_TEST_OBJ_DIR) $(UBSAN_TEST_OBJ_DIR):
	mkdir -p $@

test: test_runner
	./test_runner

test_runner: $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

asan: $(TARGET)-asan

$(TARGET)-asan: $(ASAN_OBJS)
	$(CC) $(ASAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(ASAN_LDFLAGS) $(LDLIBS)

ubsan: $(TARGET)-ubsan

$(TARGET)-ubsan: $(UBSAN_OBJS)
	$(CC) $(UBSAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(UBSAN_LDFLAGS) $(LDLIBS)

asan-test: asan-test_runner
	ASAN_OPTIONS=$(ASAN_RUNTIME_OPTIONS) ./asan-test_runner

asan-test_runner: $(ASAN_TEST_OBJS)
	$(CC) $(ASAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(ASAN_LDFLAGS) $(LDLIBS)

ubsan-test: ubsan-test_runner
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./ubsan-test_runner

ubsan-test_runner: $(UBSAN_TEST_OBJS)
	$(CC) $(UBSAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(UBSAN_LDFLAGS) $(LDLIBS)

format:
	./scripts/clang_format_apply.sh

format-check:
	./scripts/clang_format_check.sh

clang-tidy:
	@if ! command -v clang-tidy >/dev/null 2>&1; then \
		echo "clang-tidy not found" >&2; \
		exit 1; \
	fi
	clang-tidy $(SRCS) -- -I$(INC_DIR) -std=c11 -D_POSIX_C_SOURCE=200809L

cppcheck:
	@if ! command -v cppcheck >/dev/null 2>&1; then \
		echo "cppcheck not found" >&2; \
		exit 1; \
	fi
	cppcheck --enable=warning,style,performance,portability \
		--std=c11 \
		--error-exitcode=1 \
		--inline-suppr \
		--suppress=missingIncludeSystem \
		-I$(INC_DIR) \
		$(SRCS) $(INC_DIR)/*.h

static-analysis: clang-tidy cppcheck

check-version:
	./scripts/check_version_consistency.sh

docs:
	./scripts/gen_api_reference.sh
	./scripts/gen_tool_catalog.sh

docs-check:
	./scripts/gen_api_reference.sh --check
	./scripts/gen_tool_catalog.sh --check

lint: format-check docs-check check-version

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(DEBUG_TARGET) test_runner $(TARGET)-asan $(TARGET)-ubsan asan-test_runner ubsan-test_runner

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/
	install -d $(DSCO_DIR)/sessions $(DSCO_DIR)/plugins $(DSCO_DIR)/debug
	@echo "installed dsco to $(PREFIX)/bin/dsco"
	@echo "created $(DSCO_DIR)/{sessions,plugins,debug}"

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	@echo "removed $(PREFIX)/bin/$(TARGET)"

.PHONY: all debug dev clean install uninstall test docs docs-check \
	asan ubsan asan-test ubsan-test format format-check \
	lint clang-tidy cppcheck static-analysis check-version
