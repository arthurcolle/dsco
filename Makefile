CC ?= cc
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
UNAME_S := $(shell uname -s)
CC_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo yes)
ifeq ($(CC_IS_CLANG),yes)
C2Y_WARNING_FLAGS := -Wno-deprecated-octal-literals
endif

# Directories
SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
BUILD_DIR ?= build

# CI-safe defaults: allow override via env for reproducible builds
DSCO_STD ?= c2y
DSCO_ARCH ?= native
BASE_CFLAGS = -Wall -Wextra -O3 -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -D_POSIX_C_SOURCE=200809L \
	-I$(INC_DIR) \
	-march=$(DSCO_ARCH) -mtune=$(DSCO_ARCH) -funroll-loops -fvisibility=hidden \
	-funwind-tables -fno-omit-frame-pointer -g \
	-MMD -MP \
	-DBUILD_DATE='"$(BUILD_DATE)"' -DGIT_HASH='"$(GIT_HASH)"' \
	-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security \
	-Wno-error=format-security
CFLAGS ?= $(BASE_CFLAGS)
TEST_CFLAGS ?= $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline
# Release link-time optimizations:
#  -dead_strip          : drop unreferenced functions/data (smaller binary, better I-cache)
#  -dead_strip_dylibs   : drop dylibs no symbol references (gsl, gslcblas, libuv were
#                         linked-but-unused, eagerly loaded at launch; removing them
#                         cut `dsco --version` startup ~1.4ms / 1.35x — measured M4 Max).
# Applied only to the release $(TARGET) link; test/asan/ubsan keep full symbols.
RELEASE_LDFLAGS ?= -Wl,-dead_strip -Wl,-dead_strip_dylibs
# Opt-in ThinLTO: `make LTO=1`. Cross-module inlining boosts long-running
# throughput (agent loops, JSON, pipelines). Does NOT help the dyld-bound
# startup path and ~8x the link time, so it is off by default. (M4 Max:
# verified clean ThinLTO build, 81 objs in 1.5s compile + 2.5s LTO link.)
ifeq ($(LTO),1)
BASE_CFLAGS += -flto=thin
RELEASE_LDFLAGS += -flto=thin
endif
LDFLAGS ?=
LDLIBS ?= -lcurl -lsqlite3 -ldl -lm

TARGET = dsco
LITE_TARGET = dsco-lite
DEBUG_TARGET = $(TARGET)-debug

# Cosmopolitan / APE portable build lane. The default target is intentionally
# separate from $(TARGET): native DSCO keeps Darwin frameworks + Homebrew deps;
# dsco.com is the portable artifact built by scripts/cosmo_build.sh.
COSMO_TARGET ?= dsco.com
COSMOCC_VERSION ?= 4.0.2

SRC_NAMES = main.c agent.c llm.c tools.c execution_layer.c json_util.c ast.c swarm.c tui.c env_config.c \
	md.c baseline.c chronicle.c setup.c crypto.c eval.c pipeline.c plugin.c \
			semantic.c hlc.c ipc.c mcp.c mcp_names.c provider_profiles.c provider.c integrations.c error.c trace.c task_profile.c \
	output_guard.c topology.c workspace.c plan.c stateful_atoms.c recovery.c router.c \
	pheromone.c ooda.c killswitch.c governance.c memory_tier.c talons.c avian.c \
	arena_alloc.c event_loop.c vm.c scheduler.c vfs.c trading.c legion.c \
	agent_profile.c orchestrator.c vecstore.c tamper.c sealed_store.c \
	se_store.c watchdog.c audit_log.c heartbeat.c env_guard.c peer_bootstrap.c presence.c \
	project.c project_mux.c project_grid.c \
	dsco_accel.c dsco_mlx.c dsco_pool.c \
	fingerprint.c trust.c toolmgmt.c connector.c integration_fabric.c codex_app_directory.c openrouter_cache.c codex_cache.c dcr.c \
	openai_oauth.c local_llm.c \
	startup.c plot.c anim.c fractal.c shadeexpr.c face_sdf.c avatar.c self_improve.c bg_learn.c rsi_curriculum.c pets.c img_util.c supervisor.c \
	graphsub_client.c graphsub_tools.c \
	extension/backend.c extension/numerical_gsl.c extension/skill_requirements.c \
	extension/eigen_backend.c extension/fftw_backend.c extension/backend_selftest.c \
	control_flow.c \
	introspect.c \
	learned_cost.c \
	session_memory.c \
	provider_pool.c \
	math_fastpath.c \
	http_pool.c \
	$(OPTIONAL_SRCS)
TEST_SRC_NAMES = test.c

SRCS = $(addprefix $(SRC_DIR)/, $(SRC_NAMES))
# GSL vendored sources (compiled as separate objects)
GSL_OBJS = $(GSL_SRCS:gsl/src/%.c=$(OBJ_DIR)/gsl_%.o)
GSL_DEBUG_OBJS = $(GSL_SRCS:gsl/src/%.c=$(DEBUG_OBJ_DIR)/gsl_%.o)
GSL_TEST_OBJS = $(GSL_SRCS:gsl/src/%.c=$(TEST_OBJ_DIR)/gsl_%.o)
GSL_COVERAGE_OBJS = $(GSL_SRCS:gsl/src/%.c=$(TEST_COVERAGE_OBJ_DIR)/gsl_%.o)
GSL_ASAN_OBJS = $(GSL_SRCS:gsl/src/%.c=$(ASAN_OBJ_DIR)/gsl_%.o)
GSL_UBSAN_OBJS = $(GSL_SRCS:gsl/src/%.c=$(UBSAN_OBJ_DIR)/gsl_%.o)
# Test links against all src objects except main.c and agent.c
LIB_SRCS = $(filter-out $(SRC_DIR)/main.c $(SRC_DIR)/agent.c $(SRC_DIR)/orchestrator.c, $(SRCS))

OBJ_DIR := $(BUILD_DIR)/obj
DEBUG_OBJ_DIR := $(BUILD_DIR)/obj-debug
TEST_OBJ_DIR := $(BUILD_DIR)/test
TEST_COVERAGE_OBJ_DIR := $(BUILD_DIR)/coverage-test
ASAN_OBJ_DIR := $(BUILD_DIR)/asan
UBSAN_OBJ_DIR := $(BUILD_DIR)/ubsan
ASAN_TEST_OBJ_DIR := $(BUILD_DIR)/asan-test
UBSAN_TEST_OBJ_DIR := $(BUILD_DIR)/ubsan-test

OBJS = $(SRC_NAMES:%.c=$(OBJ_DIR)/%.o)
OBJS += $(GENERATED_OBJS)
LIB_OBJS = $(filter-out $(OBJ_DIR)/main.o $(OBJ_DIR)/agent.o $(OBJ_DIR)/orchestrator.o, $(OBJS))
TEST_OBJS = $(TEST_SRC_NAMES:%.c=$(TEST_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%)
TEST_COVERAGE_OBJS = $(TEST_SRC_NAMES:%.c=$(TEST_COVERAGE_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_COVERAGE_OBJ_DIR)/%)
DEBUG_OBJS = $(SRC_NAMES:%.c=$(DEBUG_OBJ_DIR)/%.o)
ASAN_OBJS = $(SRC_NAMES:%.c=$(ASAN_OBJ_DIR)/%.o)
UBSAN_OBJS = $(SRC_NAMES:%.c=$(UBSAN_OBJ_DIR)/%.o)
ASAN_TEST_OBJS = $(TEST_SRC_NAMES:%.c=$(ASAN_TEST_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(ASAN_TEST_OBJ_DIR)/%)
UBSAN_TEST_OBJS = $(TEST_SRC_NAMES:%.c=$(UBSAN_TEST_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(UBSAN_TEST_OBJ_DIR)/%)

ASAN_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -fsanitize=address
ASAN_LDFLAGS = -fsanitize=address
UBSAN_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -fsanitize=undefined -fno-sanitize-recover=all
UBSAN_LDFLAGS = -fsanitize=undefined -fno-sanitize-recover=all
DEBUG_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -DDSCO_DEV_BINARY
LITE_CFLAGS ?= -Oz -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -D_POSIX_C_SOURCE=200809L \
	-I$(INC_DIR) -DBUILD_DATE='"$(BUILD_DATE)"' -DGIT_HASH='"$(GIT_HASH)"'
COVERAGE_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline --coverage
COVERAGE_LDFLAGS = --coverage
ASAN_RUNTIME_OPTIONS = detect_leaks=1
ifeq ($(UNAME_S),Darwin)
ASAN_RUNTIME_OPTIONS = detect_leaks=0
# Secure Enclave + PAC + Touch ID + presence detection. Disabled for the
# Cosmopolitan lane: cosmocc targets the APE portable ABI, not Darwin
# Objective-C frameworks / Metal / LocalAuthentication.
ifneq ($(COSMO_BUILD),1)
BASE_CFLAGS += -DHAVE_SECURE_ENCLAVE -DHAVE_TOUCHID -mbranch-protection=standard
LDLIBS      += -framework Security -framework CoreFoundation -framework IOKit \
               -framework CoreGraphics -framework LocalAuthentication \
               -framework Foundation -framework Metal -framework MetalKit \
               -framework Accelerate

# Objective-C sources (Touch ID + Metal vecstore)
OBJC_NAMES  = touchid.m vecstore_metal.m
OBJC_SRCS   = $(addprefix $(SRC_DIR)/, $(OBJC_NAMES))
OBJC_OBJS   = $(OBJC_NAMES:%.m=$(OBJ_DIR)/%.o)
OBJS       += $(OBJC_OBJS)
DEBUG_OBJS += $(OBJC_NAMES:%.m=$(DEBUG_OBJ_DIR)/%.o)
ASAN_OBJS  += $(OBJC_NAMES:%.m=$(ASAN_OBJ_DIR)/%.o)
UBSAN_OBJS += $(OBJC_NAMES:%.m=$(UBSAN_OBJ_DIR)/%.o)
endif
endif

PREFIX ?= /opt/homebrew
DSCO_DIR = $(HOME)/.dsco
DSCO_SHARE_DIR = $(PREFIX)/share/dsco

# Detect readline
READLINE_CHECK := $(shell echo '\#include <readline/readline.h>' | $(CC) -E -x c - >/dev/null 2>&1 && echo yes)
ifeq ($(READLINE_CHECK),yes)
BASE_CFLAGS += -DHAVE_READLINE
LDLIBS += -lreadline
endif

# ── Optional libraries ────────────────────────────────────────────────────
#
# STATIC_DEPS (default 1): link small homebrew deps (hiredis, mbedtls) from
# their .a archives instead of .dylib. These dylibs live OUTSIDE the dyld
# shared cache, so each one costs a stat + mmap + codesign check at every
# process launch. Static-linking + -dead_strip removes that launch cost and
# strips unused code. Measured on M4 Max: dynamic homebrew deps cost ~1.9ms of
# a 5.7ms `dsco --version`; static hiredis+mbedtls cut startup to ~3.3ms (1.7x
# total with -dead_strip_dylibs). Set STATIC_DEPS=0 to force dylibs.
STATIC_DEPS ?= 1

# hiredis (Redis fast-path IPC)
HIREDIS_CFLAGS := $(shell pkg-config --cflags hiredis 2>/dev/null)
HIREDIS_LIBS   := $(shell pkg-config --libs   hiredis 2>/dev/null)
HIREDIS_A      := $(shell pkg-config --variable=libdir hiredis 2>/dev/null)/libhiredis.a
ifneq ($(HIREDIS_CFLAGS),)
BASE_CFLAGS += $(HIREDIS_CFLAGS) -DHAVE_REDIS
ifeq ($(STATIC_DEPS),1)
ifneq ($(wildcard $(HIREDIS_A)),)
LDLIBS      += $(HIREDIS_A)
else
LDLIBS      += $(HIREDIS_LIBS)
endif
else
LDLIBS      += $(HIREDIS_LIBS)
endif
endif

# GNU Scientific Library (vendored or system)
ifeq ($(wildcard gsl/gsl/gsl_version.h),gsl/gsl/gsl_version.h)
GSL_CFLAGS := -Igsl -DHAVE_GSL_VENDORED
GSL_LIBS   :=
GSL_SRCS   := $(wildcard gsl/src/*.c)
BASE_CFLAGS += $(GSL_CFLAGS)
$(info Using vendored GSL ($(words $(GSL_SRCS)) source files))
else
GSL_CFLAGS := $(shell pkg-config --cflags gsl 2>/dev/null)
GSL_LIBS   := $(shell pkg-config --libs   gsl 2>/dev/null)
ifneq ($(GSL_CFLAGS),)
BASE_CFLAGS += $(GSL_CFLAGS) -DHAVE_GSL
LDLIBS      += $(GSL_LIBS)
$(info Using system GSL via pkg-config)
endif
endif

# libsodium (crypto for mesh)
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null)
SODIUM_LIBS   := $(shell pkg-config --libs   libsodium 2>/dev/null)
ifneq ($(SODIUM_CFLAGS),)
BASE_CFLAGS += $(SODIUM_CFLAGS) -DHAVE_LIBSODIUM
LDLIBS      += $(SODIUM_LIBS)
endif

# libuv (async I/O event loop)
UV_CFLAGS := $(shell pkg-config --cflags libuv 2>/dev/null)
UV_LIBS   := $(shell pkg-config --libs   libuv 2>/dev/null)
ifneq ($(UV_CFLAGS),)
BASE_CFLAGS += $(UV_CFLAGS) -DHAVE_LIBUV
LDLIBS      += $(UV_LIBS)
endif

# mbedTLS 3.x (TLS server/client — no pkg-config, detect from Homebrew)
MBEDTLS_PREFIX := $(shell \
  if   [ -d /opt/homebrew/opt/mbedtls@3 ]; then echo /opt/homebrew/opt/mbedtls@3; \
  elif [ -d /usr/local/opt/mbedtls@3    ]; then echo /usr/local/opt/mbedtls@3; \
  elif [ -f /usr/include/mbedtls/ssl.h  ]; then echo /usr; \
  fi)
ifneq ($(MBEDTLS_PREFIX),)
BASE_CFLAGS += -I$(MBEDTLS_PREFIX)/include -DHAVE_MBEDTLS
ifeq ($(STATIC_DEPS),1)
ifneq ($(wildcard $(MBEDTLS_PREFIX)/lib/libmbedtls.a),)
LDLIBS      += $(MBEDTLS_PREFIX)/lib/libmbedtls.a $(MBEDTLS_PREFIX)/lib/libmbedx509.a $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a
else
LDLIBS      += -L$(MBEDTLS_PREFIX)/lib -lmbedtls -lmbedx509 -lmbedcrypto
endif
else
LDLIBS      += -L$(MBEDTLS_PREFIX)/lib -lmbedtls -lmbedx509 -lmbedcrypto
endif
endif

# Pizza-box: baked data blobs get their own flat obj names. Derive this from
# data/ rather than src/generated/, because src/generated/ may not exist until
# the bake step runs.
BAKED_DATA       := $(shell find data -maxdepth 1 -type f -print 2>/dev/null | sort)
BAKED_DATA_SYMS  := $(subst -,_,$(subst .,_,$(notdir $(BAKED_DATA))))
GENERATED_C      := $(addprefix src/generated/embedded_,$(addsuffix .c,$(BAKED_DATA_SYMS)))
GENERATED_REGISTRY := $(INC_DIR)/embedded_data_registry.h
GENERATED_OBJS   := $(patsubst src/generated/%.c,$(OBJ_DIR)/generated_%.o,$(GENERATED_C))

# Conditionally add mesh + net_server when libsodium is available
OPTIONAL_SRCS =
ifneq ($(SODIUM_CFLAGS),)
OPTIONAL_SRCS += mesh.c
OPTIONAL_SRCS += net_tool.c
OPTIONAL_SRCS += plan_optimizer.c
OPTIONAL_SRCS += cost_model.c
OPTIONAL_SRCS += plan_cache.c
OPTIONAL_SRCS += dsco_dht.c
OPTIONAL_SRCS += dht_impl.c
ifneq ($(MBEDTLS_PREFIX),)
OPTIONAL_SRCS += net_server.c
endif
endif

# Optional pkg-config libs such as GSL may include -lm. Keep one libm at the
# end of the link line so clang does not emit duplicate-library notices.
LDLIBS := $(filter-out -lm,$(LDLIBS)) -lm

all: $(TARGET) dsc dsco-new $(LITE_TARGET)
debug: $(DEBUG_TARGET)
dev: $(DEBUG_TARGET)

.PHONY: cosmo-bootstrap cosmo cosmo-run cosmo-selftest cosmo-clean cosmo-info
cosmo-bootstrap:
	chmod +x scripts/cosmo_bootstrap.sh scripts/cosmo_build.sh
	DSCO_COSMOCC_VERSION=$(COSMOCC_VERSION) scripts/cosmo_bootstrap.sh

cosmo: cosmo-bootstrap
	DSCO_COSMO_OUT=$(COSMO_TARGET) scripts/cosmo_build.sh

cosmo-run: cosmo
	./$(COSMO_TARGET) --version

cosmo-selftest: cosmo
	./$(COSMO_TARGET) --version
	./$(COSMO_TARGET) --models-json >/dev/null
	./$(COSMO_TARGET) --tools-json >/dev/null
	./$(COSMO_TARGET) --tool-exec cwd '{}' >/dev/null
	@echo "cosmo selftest ok: $(COSMO_TARGET)"

cosmo-clean:
	rm -rf build/cosmo $(COSMO_TARGET)

cosmo-info:
	@echo "COSMO_TARGET=$(COSMO_TARGET)"
	@echo "COSMOCC_VERSION=$(COSMOCC_VERSION)"
	@echo "DSCO_COSMO_MODE=$${DSCO_COSMO_MODE:-normal}"
	@echo "DSCO_COSMO_EXPERIMENTAL_FULL=$${DSCO_COSMO_EXPERIMENTAL_FULL:-0}"

# Ultra-fast edit→signal loop. Uses scripts/dev_fast.sh with a separate
# build/fast object tree, low-optimizer dev flags, dependency files, and
# optional sccache/ccache if installed.
.PHONY: fast fast-build fast-test fast-quick fast-syntax fast-changed fast-bench fast-doctor changed-tests compile-commands build-report
fast: fast-build
fast-build:
	./scripts/dev_fast.sh build
fast-test:
	./scripts/dev_fast.sh test
fast-quick:
	./scripts/dev_fast.sh quick
fast-syntax:
	./scripts/dev_fast.sh syntax
fast-changed:
	./scripts/dev_fast.sh changed
fast-bench:
	./scripts/dev_fast.sh bench
fast-doctor:
	./scripts/dev_fast.sh doctor
changed-tests:
	./scripts/changed_tests.sh
compile-commands:
	python3 scripts/gen_compile_commands.py
build-report:
	python3 scripts/build_report.py
build-cache-doctor:
	./scripts/build_cache_doctor.sh
fast-objects:
	./scripts/fast_touch.sh
time-trace:
	./scripts/build_time_trace.sh
ninja-file:
	python3 scripts/gen_ninja.py
ninja-build: ninja-file
	ninja -f build.ninja

ifeq ($(COSMO_BUILD),1)
ifeq ($(COSMO_PORTABLE),1)
$(COSMO_TARGET): $(SRC_DIR)/lite_main.c $(INC_DIR)/config.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(RELEASE_LDFLAGS) $(LDLIBS)
else
$(COSMO_TARGET): $(OBJS) $(GSL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(RELEASE_LDFLAGS) $(LDLIBS)
endif
endif

dsc: dsc.c
	$(CC) -O2 -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -D_POSIX_C_SOURCE=200809L -o $@ $< -lcurl -lreadline

$(DEBUG_TARGET): $(DEBUG_OBJS) $(GSL_DEBUG_OBJS)
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TARGET): $(OBJS) $(GSL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(RELEASE_LDFLAGS) $(LDLIBS)

# dsco-new is a twin of dsco — same code, same composer, distinct name.
dsco-new: $(TARGET)
	cp -f $(TARGET) $@

$(LITE_TARGET): $(SRC_DIR)/lite_main.c $(INC_DIR)/config.h
	$(CC) $(LITE_CFLAGS) -o $@ $<
	-strip -x $@ 2>/dev/null || true

# Source compilation rules
# ── Pizza box: bake data/ blobs before generated .o files are compiled ──
.PHONY: bake_data
bake_data: $(BUILD_DIR)/.bake_data.stamp

$(BUILD_DIR)/.bake_data.stamp: $(BAKED_DATA) scripts/bake_data.sh | $(BUILD_DIR)
	@bash scripts/bake_data.sh data src/generated include
	@touch $@

$(GENERATED_C) $(GENERATED_REGISTRY): $(BUILD_DIR)/.bake_data.stamp
	@if [ ! -f "$@" ]; then \
		rm -f $(BUILD_DIR)/.bake_data.stamp; \
		$(MAKE) --no-print-directory bake_data; \
	fi
	@test -f "$@"

# Pizza-box pattern rule: src/generated/foo.c -> build/obj/generated_foo.o
$(OBJ_DIR)/generated_%.o: src/generated/%.c | bake_data $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Pizza-box generated objects for the test/coverage/sanitizer trees. The test
# object lists inherit GENERATED_OBJS (via LIB_OBJS), so each test variant needs
# its own rule to compile src/generated/*.c into its build dir. Without these,
# `make test` and the standalone test_* targets fail with
# "No rule to make target build/test/generated_*.o".
$(TEST_OBJ_DIR)/generated_%.o: src/generated/%.c | bake_data $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_COVERAGE_OBJ_DIR)/generated_%.o: src/generated/%.c | bake_data $(TEST_COVERAGE_OBJ_DIR)
	$(CC) $(COVERAGE_CFLAGS) -c -o $@ $<

$(ASAN_TEST_OBJ_DIR)/generated_%.o: src/generated/%.c | bake_data $(ASAN_TEST_OBJ_DIR)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(UBSAN_TEST_OBJ_DIR)/generated_%.o: src/generated/%.c | bake_data $(UBSAN_TEST_OBJ_DIR)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Memory-bounded compilation of large translation units ──────────────────
# tools.c (>1MB of source), agent.c, tui.c, integrations.c, trading.c, llm.c,
# provider.c, md.c and topology.c are huge dispatch/glue units. Newer
# persistence/orchestration/network glue is also not hot numeric code and can
# contribute to peak clang RSS during parallel builds. At
# -O3 -funroll-loops -march=native clang's inliner/optimizer needs multiple GB
# of RAM *per file*; a parallel `make -jN` compiling several at once exhausts
# RAM+swap and the kernel SIGKILLs the build (and other resident processes).
# These are not hot numeric paths, so -O1 costs ~nothing at runtime while
# cutting peak compiler RSS ~5-8x. Hot numeric code (gsl/, extension/) keeps -O3.
BIG_TU_NAMES = tools agent tui integrations trading llm provider md topology \
	session_memory plan_optimizer cost_model plan_cache dsco_dht dht_impl \
	net_server vecstore_metal
BIG_TU_OBJS  = $(BIG_TU_NAMES:%=$(OBJ_DIR)/%.o)
$(BIG_TU_OBJS): CFLAGS := $(filter-out -O3 -funroll-loops,$(CFLAGS)) -O1

# Objective-C sources (macOS only)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

# Vendored GSL source compilation rules
$(OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(DEBUG_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(DEBUG_OBJ_DIR)
	$(CC) $(DEBUG_CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_COVERAGE_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(TEST_COVERAGE_OBJ_DIR)
	$(CC) $(COVERAGE_CFLAGS) -c -o $@ $<

$(ASAN_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(ASAN_OBJ_DIR)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(UBSAN_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(UBSAN_OBJ_DIR)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

$(DEBUG_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(DEBUG_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(DEBUG_CFLAGS) -c -o $@ $<

$(DEBUG_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(DEBUG_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(DEBUG_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

$(ASAN_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(ASAN_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(ASAN_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(ASAN_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

$(UBSAN_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(UBSAN_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

$(UBSAN_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(UBSAN_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(UBSAN_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

# Test compilation rules — test sources from tests/, lib sources from src/
$(TEST_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/test_%.o: $(TEST_DIR)/test_%.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

$(TEST_COVERAGE_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(TEST_COVERAGE_OBJ_DIR)
	$(CC) $(COVERAGE_CFLAGS) -c -o $@ $<

$(TEST_COVERAGE_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(TEST_COVERAGE_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(COVERAGE_CFLAGS) -c -o $@ $<

$(TEST_COVERAGE_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(TEST_COVERAGE_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(COVERAGE_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

$(ASAN_TEST_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(ASAN_TEST_OBJ_DIR)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(ASAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(ASAN_TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(ASAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(ASAN_TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

$(UBSAN_TEST_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(UBSAN_TEST_OBJ_DIR)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

$(UBSAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(UBSAN_TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

$(UBSAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(UBSAN_TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(UBSAN_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

$(OBJ_DIR) $(DEBUG_OBJ_DIR) $(TEST_OBJ_DIR) $(TEST_COVERAGE_OBJ_DIR) $(ASAN_OBJ_DIR) $(UBSAN_OBJ_DIR) $(ASAN_TEST_OBJ_DIR) $(UBSAN_TEST_OBJ_DIR):
	mkdir -p $@
	mkdir -p $@/extension

# Header dependency tracking: -MMD -MP (in BASE_CFLAGS) emits a .d file next to
# each .o listing the headers it included. Including them here makes any object
# rebuild when a header it uses changes — e.g. editing include/config.h now
# correctly recompiles every .o that includes it, instead of silently shipping
# a stale binary.
-include $(wildcard $(BUILD_DIR)/*/*.d)

test: test_runner
	./test_runner

test_runner: $(TEST_OBJS) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# ── TUI snapshot tests (Integument golden tests) ─────────────────────────
# Headless golden tests for deterministic render primitives. These link the
# test-object graph (main/agent/orchestrator excluded by LIB_OBJS) plus vendored
# GSL, and provide test-local globals for CLI-entry symbols.
TUI_TEST_LIB_OBJS = $(LIB_OBJS) $(GSL_OBJS)

.PHONY: test_tui_snapshot test_tui_theme_snapshot test_tui_snapshots

test_tui_snapshot: $(TEST_OBJ_DIR)/test_tui_snapshot.o $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_tui_theme_snapshot: $(TEST_OBJ_DIR)/test_tui_theme_snapshot.o $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_tui_snapshots: test_tui_snapshot test_tui_theme_snapshot

# Priority 7 standalone test binary
RECOVERY_TEST_OBJS = $(TEST_OBJ_DIR)/test_recovery.o \
	$(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%)

$(TEST_OBJ_DIR)/test_recovery.o: $(TEST_DIR)/test_recovery.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

test_recovery: $(RECOVERY_TEST_OBJS) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_stateful_atoms: $(TEST_OBJ_DIR)/test_stateful_atoms.o \
	$(TEST_OBJ_DIR)/stateful_atoms.o \
	$(TEST_OBJ_DIR)/plan.o \
	$(TEST_OBJ_DIR)/json_util.o \
	$(TEST_OBJ_DIR)/arena_alloc.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

$(TEST_OBJ_DIR)/test_stateful_atoms.o: $(TEST_DIR)/test_stateful_atoms.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# Priority standalone test binaries (P1/P3/P4/P5/P6). Each test_*.c has its own
# main() (so they can't fold into test_runner) and a self-contained stub for any
# runtime globals it needs. They therefore link a MINIMAL object set — only the
# module under test plus its direct deps — to avoid duplicate-symbol clashes
# with tools.o/agent.o that define the same globals. Mirrors test_stateful_atoms.

$(TEST_OBJ_DIR)/test_plan_optimizer.o: $(TEST_DIR)/test_plan_optimizer.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
# plan_optimizer pulls in topology→swarm→provider→plan_cache; link full lib set.
test_plan_optimizer: $(TEST_OBJ_DIR)/test_plan_optimizer.o \
	$(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TEST_OBJ_DIR)/test_plan_cache.o: $(TEST_DIR)/test_plan_cache.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_plan_cache: $(TEST_OBJ_DIR)/test_plan_cache.o \
	$(TEST_OBJ_DIR)/plan_cache.o $(TEST_OBJ_DIR)/json_util.o \
	$(TEST_OBJ_DIR)/arena_alloc.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

$(TEST_OBJ_DIR)/test_learned_cost.o: $(TEST_DIR)/test_learned_cost.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_learned_cost: $(TEST_OBJ_DIR)/test_learned_cost.o \
	$(TEST_OBJ_DIR)/learned_cost.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

$(TEST_OBJ_DIR)/test_session_memory.o: $(TEST_DIR)/test_session_memory.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
# session_memory pulls in vecstore + tools_embed_text; link full lib set.
test_session_memory: $(TEST_OBJ_DIR)/test_session_memory.o \
	$(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TEST_OBJ_DIR)/test_control_flow.o: $(TEST_DIR)/test_control_flow.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_control_flow: $(TEST_OBJ_DIR)/test_control_flow.o \
	$(TEST_OBJ_DIR)/control_flow.o $(TEST_OBJ_DIR)/plan.o \
	$(TEST_OBJ_DIR)/json_util.o $(TEST_OBJ_DIR)/arena_alloc.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

$(TEST_OBJ_DIR)/test_avian.o: $(TEST_DIR)/test_avian.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_avian: $(TEST_OBJ_DIR)/test_avian.o $(TEST_OBJ_DIR)/avian.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Math fast-path corpus test. Links the REAL production logic (math_fastpath.c
# + eval.c) and validates routing + value over thousands of generated cases.
# Regenerates the corpus first so it can never drift from the generator.
$(TEST_OBJ_DIR)/test_math_corpus.o: $(TEST_DIR)/test_math_corpus.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

test_math_corpus: $(TEST_OBJ_DIR)/test_math_corpus.o \
	$(TEST_OBJ_DIR)/math_fastpath.o $(TEST_OBJ_DIR)/eval.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm
	python3 $(TEST_DIR)/gen_math_corpus.py $(TEST_DIR)/math_corpus.tsv
	./test_math_corpus $(TEST_DIR)/math_corpus.tsv

# Build + run every standalone priority test in sequence.
.PHONY: test_priorities
test_priorities: test_recovery test_stateful_atoms test_plan_optimizer test_plan_cache \
	test_learned_cost test_session_memory test_control_flow test_avian test_math_corpus
	./test_recovery
	./test_stateful_atoms
	./test_plan_optimizer
	./test_plan_cache
	./test_learned_cost
	./test_session_memory
	./test_control_flow
	./test_avian
	./test_math_corpus $(TEST_DIR)/math_corpus.tsv

coverage: coverage_runner
	./coverage_runner

coverage_runner: $(TEST_COVERAGE_OBJS) $(GSL_COVERAGE_OBJS)
	$(CC) $(COVERAGE_CFLAGS) -o $@ $^ $(LDFLAGS) $(COVERAGE_LDFLAGS) $(LDLIBS)

asan: $(TARGET)-asan

$(TARGET)-asan: $(ASAN_OBJS) $(GSL_ASAN_OBJS)
	$(CC) $(ASAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(ASAN_LDFLAGS) $(LDLIBS)

ubsan: $(TARGET)-ubsan

$(TARGET)-ubsan: $(UBSAN_OBJS) $(GSL_UBSAN_OBJS)
	$(CC) $(UBSAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(UBSAN_LDFLAGS) $(LDLIBS)

asan-test: asan-test_runner
	ASAN_OPTIONS=$(ASAN_RUNTIME_OPTIONS) ./asan-test_runner

asan-test_runner: $(ASAN_TEST_OBJS) $(GSL_ASAN_OBJS)
	$(CC) $(ASAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(ASAN_LDFLAGS) $(LDLIBS)

ubsan-test: ubsan-test_runner
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./ubsan-test_runner

ubsan-test_runner: $(UBSAN_TEST_OBJS) $(GSL_UBSAN_OBJS)
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
	clang-tidy $(SRCS) -- -I$(INC_DIR) -std=c11 -D_POSIX_C_SOURCE=200809L -DHAVE_MBEDTLS -DHAVE_LIBSODIUM -DHAVE_LIBUV

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
	python3 scripts/index_constants_env.py --root .

docs-check:
	./scripts/gen_api_reference.sh --check
	./scripts/gen_tool_catalog.sh --check
	python3 scripts/index_constants_env.py --root . --check

bench-startup: $(TARGET) dsc
	@echo "== dsco metadata startup =="
	@/usr/bin/time -p sh -c './$(TARGET) --version >/dev/null 2>&1'
	@/usr/bin/time -p sh -c './$(TARGET) --help >/dev/null 2>&1'
	@/usr/bin/time -p sh -c './$(TARGET) --models-json >/dev/null 2>&1'
	@echo "== dsc metadata startup =="
	@/usr/bin/time -p sh -c './dsc --help >/dev/null 2>&1'

bench-tool: $(TARGET)
	@echo "== dsco direct local tool execution =="
	@/usr/bin/time -p sh -c './$(TARGET) --tool-exec cwd "{}" >/dev/null 2>&1'
	@DSCO_PERF=1 ./$(TARGET) --tool-exec cwd '{}' >/dev/null

bench-agent-loop: $(TARGET)
	@echo "== dsco provider benchmark =="
	@DSCO_CHEAP=1 DSCO_PERF=1 ./$(TARGET) -C -e bench "$${DSCO_BENCH_PROMPT:-Reply with exactly DSCOPERFOK}"

bench-local: bench-startup bench-tool

bench-sota: $(TARGET) $(LITE_TARGET)
	@python3 scripts/bench_sota.py

bench-ttft: $(TARGET)
	@if [ "$${DSCO_RUN_NETWORK_BENCH}" = "1" ]; then \
		DSCO_PERF=json ./$(TARGET) --profile lite -C -e bench "$${DSCO_BENCH_PROMPT:-Reply with exactly DSCOTTFTOK}"; \
	else \
		printf '{"bench":"ttft","status":"skipped","reason":"set DSCO_RUN_NETWORK_BENCH=1"}\n'; \
	fi

bench-worker: $(LITE_TARGET)
	@printf '{"bench":"worker","case":"worker-lite-version","phase":"start"}\n'
	@DSCO_WORKER=1 ./$(LITE_TARGET) --version >/dev/null
	@printf '{"bench":"worker","case":"worker-lite-version","status":"ok"}\n'

bench-size: $(TARGET) $(LITE_TARGET)
	@printf '{"bench":"size","binary":"%s","bytes":%s}\n' "$(TARGET)" "$$(wc -c < ./$(TARGET))"
	@printf '{"bench":"size","binary":"%s","bytes":%s}\n' "$(LITE_TARGET)" "$$(wc -c < ./$(LITE_TARGET))"

lint: format-check docs-check check-version

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(LITE_TARGET) $(DEBUG_TARGET) dsc test_runner coverage_runner $(TARGET)-asan $(TARGET)-ubsan asan-test_runner ubsan-test_runner

install: $(TARGET) $(LITE_TARGET) dsc
	install -d $(PREFIX)/bin
	install -d $(DSCO_SHARE_DIR)
	install -m 755 $(TARGET) $(PREFIX)/bin/
	install -m 755 $(LITE_TARGET) $(PREFIX)/bin/
	install -m 755 dsc $(PREFIX)/bin/
	install -m 755 scripts/live_face_avatar.sh $(PREFIX)/bin/dsco-live-face-avatar
	test -f dsco-new && install -m 755 dsco-new $(PREFIX)/bin/ || true
	install -m 644 $(INC_DIR)/tool_embeddings.bin $(DSCO_SHARE_DIR)/
	install -m 755 face_capture.py $(DSCO_SHARE_DIR)/
	install -d $(DSCO_DIR)/sessions $(DSCO_DIR)/plugins $(DSCO_DIR)/debug
	@echo "installed dsco, dsco-lite, dsc, dsco-new to $(PREFIX)/bin/"
	@echo "installed tool_embeddings.bin to $(DSCO_SHARE_DIR)/"
	@echo "installed dsco-live-face-avatar and face_capture.py"
	@echo "created $(DSCO_DIR)/{sessions,plugins,debug}"

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	rm -f $(PREFIX)/bin/$(LITE_TARGET)
	rm -f $(PREFIX)/bin/dsc
	rm -f $(PREFIX)/bin/dsco-new
	rm -f $(PREFIX)/bin/dsco-live-face-avatar
	rm -f $(DSCO_SHARE_DIR)/tool_embeddings.bin
	rm -f $(DSCO_SHARE_DIR)/face_capture.py
	-rmdir $(DSCO_SHARE_DIR) 2>/dev/null || true
	@echo "removed $(PREFIX)/bin/$(TARGET)"

ui-deps:
	pip install -r web/requirements.txt

ui: $(TARGET) ui-deps
	./$(TARGET) --ui

.PHONY: all debug dev clean install uninstall test coverage docs docs-check \
	asan ubsan asan-test ubsan-test format format-check \
	fast fast-build fast-test fast-quick fast-syntax fast-changed fast-bench fast-doctor \
	changed-tests compile-commands build-report build-cache-doctor fast-objects time-trace ninja-file ninja-build \
	lint clang-tidy cppcheck static-analysis check-version \
	ui ui-deps bench-startup bench-tool bench-agent-loop bench-local \
	bench-sota bench-ttft bench-worker bench-size
