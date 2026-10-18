CC ?= cc
# Pin the default goal: prerequisite-only rules (e.g. native_voice_macos.o: plist)
# appear before `all:` and would otherwise hijack a bare `make`.
.DEFAULT_GOAL := all
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
CC_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo yes)
ifeq ($(CC_IS_CLANG),yes)
C2Y_WARNING_FLAGS := -Wno-deprecated-octal-literals
endif

# Directories
SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
BUILD_DIR ?= build

# CI-safe defaults: allow override via env for reproducible builds.
# Newest C standard the toolchain accepts: recent Apple clang knows c2y, but
# CI's gcc-13/clang-18 only know c23/c2x — probe once at parse time.
# Simply expanded so the probe runs once, not per recipe (a recursive `?=`
# re-ran it for every compile and an intermittent probe failure under -j left
# `-std=` empty).
ifeq ($(origin DSCO_STD),undefined)
DSCO_STD := $(shell for s in c2y c23 c2x c11; do \
	if $(CC) -std=$$s -x c -c /dev/null -o /dev/null 2>/dev/null; then echo $$s; break; fi; done)
endif
DSCO_ARCH ?= native
# clang/gcc reject -march=arm64 (the CI matrix passes arch names, not ISA
# levels); arm64 targets get default codegen instead.
ifneq (,$(filter $(DSCO_ARCH),arm64 aarch64))
DSCO_ARCH_FLAGS :=
else
DSCO_ARCH_FLAGS := -march=$(DSCO_ARCH) -mtune=$(DSCO_ARCH)
endif
BASE_CFLAGS = -Wall -Wextra -O3 -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
	-I$(INC_DIR) \
	$(DSCO_ARCH_FLAGS) -funroll-loops -fvisibility=hidden \
	-funwind-tables -fno-omit-frame-pointer -g \
	-MMD -MP \
	-DBUILD_DATE='"$(BUILD_DATE)"' -DGIT_HASH='"$(GIT_HASH)"' \
	-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security \
	-Wno-error=format-security
CFLAGS ?= $(BASE_CFLAGS)
TEST_CFLAGS ?= $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline
override TEST_CFLAGS += -DDSCO_INTERNAL_TESTS
# Release link-time optimizations:
#  -dead_strip          : drop unreferenced functions/data (smaller binary, better I-cache)
#  -dead_strip_dylibs   : drop dylibs no symbol references (gsl, gslcblas, libuv were
#                         linked-but-unused, eagerly loaded at launch; removing them
#                         cut `dsco --version` startup ~1.4ms / 1.35x — measured M4 Max).
# Applied only to the release $(TARGET) link; test/asan/ubsan keep full symbols.
ifeq ($(UNAME_S),Darwin)
RELEASE_LDFLAGS ?= -Wl,-dead_strip -Wl,-dead_strip_dylibs
else
# GNU ld has no -dead_strip; --gc-sections is the closest equivalent
RELEASE_LDFLAGS ?= -Wl,--gc-sections
endif
# Opt-in ThinLTO: `make LTO=1`. Cross-module inlining boosts long-running
# throughput (agent loops, JSON, pipelines). Does NOT help the dyld-bound
# startup path and ~8x the link time, so it is off by default. (M4 Max:
# verified clean ThinLTO build, 81 objs in 1.5s compile + 2.5s LTO link.)
ifeq ($(LTO),1)
BASE_CFLAGS += -flto=thin
RELEASE_LDFLAGS += -flto=thin
endif
LDFLAGS ?=
LDLIBS ?= -lcurl -lsqlite3 -ldl -lz -lm
ifeq ($(shell uname -s),Linux)
LDLIBS += -lutil -lpthread
endif

TARGET = dsco
LITE_TARGET = dsco-lite
SPINE_TARGET = spine-dsco-slim
WASM_TARGET = web/static/dsco_wasm.js
WASM_EXPORTS = '["_dsco_wasm_version","_dsco_wasm_exports_json","_dsco_wasm_models_json","_dsco_wasm_tools_json","_dsco_wasm_route_explain","_dsco_wasm_tool_exec","_dsco_wasm_session_reset","_dsco_wasm_session_add","_dsco_wasm_session_state"]'
WASM_CACHE_DIR ?= $(BUILD_DIR)/emscripten-cache
WASM_CACHE_ABS := $(abspath $(WASM_CACHE_DIR))
DEBUG_TARGET = $(TARGET)-debug
PROFILE_TARGET ?= dsco-instrumented

# Cosmopolitan / APE portable build lane. The default target is the hosted
# binary artifact name and is intentionally separate from $(TARGET): native DSCO
# keeps Darwin frameworks + Homebrew deps.
COSMO_TARGET ?= dsco.distributed.systems
COSMO_LEGACY_TARGET ?= dsco.com
COSMOCC_VERSION ?= 4.0.2

SRC_NAMES = main.c worldsim.c agent.c llm.c codex_tool_bridge.c input_budget.c tools.c tool_effects.c execution_layer.c execution_kernel.c execution_events.c event_stream.c provider_events.c stream_diagnostics.c execution_recovery.c headless_accounting.c inference_cost.c json_util.c ast.c swarm.c worker_runtime.c swarm_progress.c swarm_scale.c swarm_hierarchy.c swarm_accounting.c swarm_telemetry.c machine_society.c swarm_daemon.c tui.c tui_swarm_dock.c native_window_layout.c native_windows.c native_session_details.c native_startup_card.c native_factory.c native_task_service.c native_workbench.c native_window_tool.c native_ui.c native_ui_json.c pixel_tui.c pixel_tui_perf.c pixel_fx.c pixel_hdr.c native_kinetic.c ui_motion.c kitty_graphics.c presentation_blocks.c native_table_blocks.c presentation_json.c transcript_view.c rich_text.c font_compat.c kitty_tools.c kitty_agent_windows.c process_capture.c tool_content.c tool_grounding.c surface_policy.c surface_registry.c surface_cli.c buffer_store.c buffer_view.c buffer_textedit.c buffer_cli.c ide_cli.c buffer_ui.c pty_session.c desktop_macos.c app_control.c vision_ground.c computer_screen.c browser_session.c env_config.c \
	px_backend.c px_theme.c native_composer.c native_display.c native_masthead.c compositor_parity.c compositor_stream_bench.c native_buffer_editor.c native_trace.c native_trace_ui.c \
	edit_diff.c code_edit.c skill_lifecycle.c fuzzy_match.c critical_stamp.c dsco_c.c config_surface.c work_result.c work_unit.c work_algebra.c work_dispatch.c execution_plan.c env_sense.c result_spill.c md.c rtf.c baseline.c chronicle.c agent_event.c callbacks.c setup.c crypto.c eval.c pipeline.c plugin.c kitty_banner.c \
			semantic.c hlc.c ipc.c mcp.c mcp_response.c mcp_stream.c cancel_wait.c mcp_diagnostics.c mcp_server.c mcp_names.c provider_profiles.c abliteration.c provider.c provider_responses.c provider_transport.c integrations.c error.c trace.c instrumenter.c structured_process.c task_profile.c \
	output_guard.c topology.c workspace.c directive_store.c value_ledger.c plan.c stateful_atoms.c recovery.c router.c \
		durable_agents.c bus_cli.c skills_cli.c skill_index.c skill_candidate.c skill_trace.c \
	shell_jobs.c capability.c tool_hooks.c \
	pheromone.c ooda.c overmind.c killswitch.c governance.c gov_experiment.c memory_tier.c talons.c avian.c \
	arena_alloc.c event_loop.c swarm_reactor.c vm.c scheduler.c scheduler_lifetime.c scheduler_io.c waiter.c vfs.c trading.c legion.c \
	agent_profile.c orchestrator.c vecstore.c tamper.c sealed_store.c harden.c embedded_data.c cstring_unlock.c \
	se_store.c watchdog.c audit_log.c heartbeat.c env_guard.c peer_bootstrap.c presence.c \
	project.c project_mux.c project_grid.c \
	dsco_accel.c dsco_mlx.c dsco_pool.c \
	fingerprint.c trust.c tool_telemetry.c trace_kg.c trace_kg_store.c toolmgmt.c connector.c integration_fabric.c codex_app_directory.c openrouter_cache.c codex_cache.c codex_usage.c dcr.c \
	openai_oauth.c kimi_oauth.c company_account.c local_llm.c model_pricing.c parallel_pricing.c cost_frontier.c deepseek_pricing.c model_catalog_refresh.c model_official_metadata.c subscription_gate.c provider_admission.c subscription_bench.c subscription_paths.c auth_lanes.c \
	startup.c plot.c anim.c fractal.c shadeexpr.c face_sdf.c avatar.c self_improve.c bg_learn.c autoresearch.c rsi_curriculum.c pets.c img_util.c supervisor.c ring_buffer.c \
	graphsub_client.c graphsub_tools.c graphsub_operator.c lingo_graphsub_world.c lingo_autobot.c lingo_chimera.c lingo_workflow.c service_boundary.c \
	openai_images.c \
	webhook_security.c \
	extension/backend.c extension/numerical_gsl.c extension/skill_requirements.c \
	extension/eigen_backend.c extension/fftw_backend.c extension/backend_selftest.c \
	control_flow.c \
	introspect.c \
	chimera_scale.c \
	learned_cost.c \
	spend_governor.c \
	seat_take.c \
	jev_decide.c \
	realtime_jev.c \
	jev_gates.c \
	compact_policy.c \
	request_context.c \
	request_intent.c \
	provider_continuity.c \
	ai_repl.c \
	repl_sem.c \
	agent_consensus.c \
	frontier.c \
	executive.c \
	strategy.c \
	command_plane.c \
	plan_dag.c \
	session_memory.c \
	provider_pool.c \
	dsco_swim.c improvement_sync.c weather_batch.c openrouter_lanes.c sequence_state.c \
	math_fastpath.c \
	http_pool.c \
	realtime.c voice_session.c \
	remote_cli.c \
	cluster.c \
	activation_lease.c \
	cloud_runtime.c context_fabric.c context_eviction.c jev_compaction.c jev_router.c prompt_branch.c capsule.c acp_server.c agent_interop.c agent_run_outcome.c task_closeout.c goal.c goal_queue.c goal_native.c tool_assurance.c headless_tool_guard.c execution_contract.c \
           blackboard.c lingo.c lingo_origin.c lingo_session.c lingo_workbench.c json_fast.c \
	construct.c prompt_pool.c rl_hooks.c \
	$(OPTIONAL_SRCS)
SRC_NAMES += timeout_policy.c swarm_lifecycle.c turn_record.c task_profile_tool.c native_voice.c json_tools.c event_wakeup.c shell_job_events.c
SRC_NAMES += native_followup_queue.c
SRC_NAMES += async_runtime.c async_join.c async_pool.c
SRC_NAMES += context_compaction.c context_archive.c tool_integrity_index.c
SRC_NAMES += crypto_primitives.c rollout.c
SRC_NAMES += eval_suite.c

ASYNC_STRUCTURE_SRCS = tests/test_async_structures.c src/async_runtime.c src/async_join.c src/async_pool.c src/dsco_pool.c src/event_loop.c src/event_wakeup.c
ASYNC_LIBRARY_NAMES = async_runtime async_join async_pool dsco_pool event_loop event_wakeup
ASYNC_LIBRARY_OBJS = $(addprefix $(BUILD_DIR)/async-library/,$(addsuffix .o,$(ASYNC_LIBRARY_NAMES)))

TEST_SRC_NAMES = test.c

SRC_NAMES += native_app.c native_app_store.c native_workbench_state.c
SRCS = $(addprefix $(SRC_DIR)/, $(SRC_NAMES))
# GSL vendored sources (compiled as separate objects)
GSL_OBJS = $(GSL_SRCS:gsl/src/%.c=$(OBJ_DIR)/gsl_%.o)
GSL_DEBUG_OBJS = $(GSL_SRCS:gsl/src/%.c=$(DEBUG_OBJ_DIR)/gsl_%.o)
GSL_TEST_OBJS = $(GSL_SRCS:gsl/src/%.c=$(TEST_OBJ_DIR)/gsl_%.o)
GSL_COVERAGE_OBJS = $(GSL_SRCS:gsl/src/%.c=$(TEST_COVERAGE_OBJ_DIR)/gsl_%.o)
GSL_ASAN_OBJS = $(GSL_SRCS:gsl/src/%.c=$(ASAN_OBJ_DIR)/gsl_%.o)
GSL_ASAN_TEST_OBJS = $(GSL_SRCS:gsl/src/%.c=$(ASAN_TEST_OBJ_DIR)/gsl_%.o)
GSL_UBSAN_OBJS = $(GSL_SRCS:gsl/src/%.c=$(UBSAN_OBJ_DIR)/gsl_%.o)
GSL_UBSAN_TEST_OBJS = $(GSL_SRCS:gsl/src/%.c=$(UBSAN_TEST_OBJ_DIR)/gsl_%.o)
GSL_TSAN_TEST_OBJS = $(GSL_SRCS:gsl/src/%.c=$(TSAN_TEST_OBJ_DIR)/gsl_%.o)
GSL_ASAN_UBSAN_TEST_OBJS = $(GSL_SRCS:gsl/src/%.c=$(ASAN_UBSAN_TEST_OBJ_DIR)/gsl_%.o)
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
TSAN_TEST_OBJ_DIR := $(BUILD_DIR)/tsan-test
ASAN_UBSAN_TEST_OBJ_DIR := $(BUILD_DIR)/asan-ubsan-test

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
TSAN_TEST_OBJS = $(TEST_SRC_NAMES:%.c=$(TSAN_TEST_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(TSAN_TEST_OBJ_DIR)/%)
ASAN_UBSAN_TEST_OBJS = $(TEST_SRC_NAMES:%.c=$(ASAN_UBSAN_TEST_OBJ_DIR)/%.o) $(LIB_OBJS:$(OBJ_DIR)/%=$(ASAN_UBSAN_TEST_OBJ_DIR)/%)

SANITIZER_BASE_CFLAGS = $(filter-out -D_FORTIFY_SOURCE=2,$(BASE_CFLAGS))

ASAN_CFLAGS = $(SANITIZER_BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -fsanitize=address
override ASAN_CFLAGS += -DDSCO_INTERNAL_TESTS
ASAN_LDFLAGS = -fsanitize=address
UBSAN_CFLAGS = $(SANITIZER_BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -fsanitize=undefined -fno-sanitize-recover=all
override UBSAN_CFLAGS += -DDSCO_INTERNAL_TESTS
UBSAN_LDFLAGS = -fsanitize=undefined -fno-sanitize-recover=all
TSAN_CFLAGS = $(SANITIZER_BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -fsanitize=thread
override TSAN_CFLAGS += -DDSCO_INTERNAL_TESTS
TSAN_LDFLAGS = -fsanitize=thread
ASAN_UBSAN_CFLAGS = $(SANITIZER_BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -fsanitize=address,undefined -fno-sanitize-recover=all
override ASAN_UBSAN_CFLAGS += -DDSCO_INTERNAL_TESTS
ASAN_UBSAN_LDFLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all
DEBUG_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -DDSCO_DEV_BINARY
PROFILE_COVERAGE_FLAGS = -finstrument-functions -fsanitize-coverage=trace-pc-guard,trace-cmp,indirect-calls,trace-div,trace-gep
PROFILE_CFLAGS = $(BASE_CFLAGS) -O1 -g -fno-omit-frame-pointer -fno-inline \
	-fno-optimize-sibling-calls -DDSCO_OBJECT_INSTRUMENTATION $(PROFILE_COVERAGE_FLAGS)
ifeq ($(PROFILE_BUILD),1)
override CFLAGS = $(PROFILE_CFLAGS)
endif
LITE_CFLAGS ?= -Oz -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
	-I$(INC_DIR) -DBUILD_DATE='"$(BUILD_DATE)"' -DGIT_HASH='"$(GIT_HASH)"'
COVERAGE_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline --coverage
override COVERAGE_CFLAGS += -DDSCO_INTERNAL_TESTS
COVERAGE_LDFLAGS = --coverage
# Leak checking is off on every platform until a dedicated leak burndown:
# the suite has never run under LSan and end-of-process leaks would drown the
# address-error signal ASan is here for. `make leak-test` enables it explicitly.
ASAN_RUNTIME_OPTIONS ?= detect_leaks=0
ifeq ($(UNAME_S),Darwin)
# Secure Enclave + PAC + Touch ID + presence detection. Disabled for the
# Cosmopolitan lane: cosmocc targets the APE portable ABI, not Darwin
# Objective-C frameworks / Metal / LocalAuthentication.
ifneq ($(COSMO_BUILD),1)
BASE_CFLAGS += -DHAVE_SECURE_ENCLAVE -DHAVE_TOUCHID
# PAC/BTI branch protection is arm64-only; Intel Macs reject the flag
ifeq ($(UNAME_M),arm64)
BASE_CFLAGS += -mbranch-protection=standard
endif
LDLIBS      += -framework Security -framework CoreFoundation -framework IOKit -framework DiskArbitration \
               -framework ApplicationServices -framework CoreGraphics -framework CoreText -framework LocalAuthentication \
               -framework Foundation -framework Metal -framework MetalKit \
               -framework Accelerate -framework AudioToolbox -framework AVFoundation -framework Speech
LDFLAGS     += -Wl,-sectcreate,__TEXT,__info_plist,$(CURDIR)/scripts/native_voice_info.plist

# Objective-C sources (Touch ID + Metal vecstore + on-device voice)
OBJC_NAMES  = touchid.m vecstore_metal.m native_voice_macos.m
# Usage-description changes must force a fresh Mach-O even without C edits.
$(OBJ_DIR)/native_voice_macos.o $(DEBUG_OBJ_DIR)/native_voice_macos.o $(ASAN_OBJ_DIR)/native_voice_macos.o $(UBSAN_OBJ_DIR)/native_voice_macos.o: scripts/native_voice_info.plist
OBJC_SRCS   = $(addprefix $(SRC_DIR)/, $(OBJC_NAMES))
OBJC_OBJS   = $(OBJC_NAMES:%.m=$(OBJ_DIR)/%.o)
OBJS       += $(OBJC_OBJS)
DEBUG_OBJS += $(OBJC_NAMES:%.m=$(DEBUG_OBJ_DIR)/%.o)
ASAN_OBJS  += $(OBJC_NAMES:%.m=$(ASAN_OBJ_DIR)/%.o)
UBSAN_OBJS += $(OBJC_NAMES:%.m=$(UBSAN_OBJ_DIR)/%.o)
endif
endif

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
DSCO_INSTALL_SYNC_PATH ?= auto
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

# LuaJIT is optional; the Lingo tool reports a clear build requirement if absent.
LUAJIT_CFLAGS := $(shell pkg-config --cflags luajit 2>/dev/null)
LUAJIT_LIBS := $(shell pkg-config --libs luajit 2>/dev/null)
ifneq ($(LUAJIT_LIBS),)
BASE_CFLAGS += $(LUAJIT_CFLAGS) -DHAVE_LUAJIT
LDLIBS += $(LUAJIT_LIBS)
endif

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

# libsodium (crypto for mesh). Detect via --exists: --cflags is empty when
# headers live in the default include path (e.g. apt), which is not "absent".
SODIUM_FOUND  := $(shell pkg-config --exists libsodium 2>/dev/null && echo yes)
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null)
SODIUM_LIBS   := $(shell pkg-config --libs   libsodium 2>/dev/null)
ifeq ($(SODIUM_FOUND),yes)
BASE_CFLAGS += $(SODIUM_CFLAGS) -DHAVE_LIBSODIUM
LDLIBS      += $(SODIUM_LIBS)
endif

# Optional PQ backend for the public primitive layer only. Existing legacy
# callers retain their algorithms and wire formats. No library installation.
OQS_FOUND := $(shell pkg-config --exists liboqs 2>/dev/null && echo yes)
ifeq ($(OQS_FOUND),yes)
CRYPTO_PRIMITIVE_CFLAGS := $(shell pkg-config --cflags liboqs 2>/dev/null) -DHAVE_LIBOQS
LDLIBS += $(shell pkg-config --libs --static liboqs 2>/dev/null)
endif
$(OBJ_DIR)/crypto_primitives.o $(DEBUG_OBJ_DIR)/crypto_primitives.o $(ASAN_OBJ_DIR)/crypto_primitives.o $(UBSAN_OBJ_DIR)/crypto_primitives.o: CFLAGS += $(CRYPTO_PRIMITIVE_CFLAGS)

# libuv (async I/O event loop)
UV_FOUND  := $(shell pkg-config --exists libuv 2>/dev/null && echo yes)
UV_CFLAGS := $(shell pkg-config --cflags libuv 2>/dev/null)
UV_LIBS   := $(shell pkg-config --libs   libuv 2>/dev/null)
ifeq ($(UV_FOUND),yes)
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
BAKED_DATA       := $(shell find data -maxdepth 1 -type f ! -name '.*' -print 2>/dev/null | sort)
BAKED_DATA_SYMS  := $(subst -,_,$(subst .,_,$(notdir $(BAKED_DATA))))
GENERATED_C      := $(addprefix src/generated/embedded_,$(addsuffix .c,$(BAKED_DATA_SYMS)))
GENERATED_REGISTRY := $(INC_DIR)/embedded_data_registry.h
GENERATED_OBJS   := $(patsubst src/generated/%.c,$(OBJ_DIR)/generated_%.o,$(GENERATED_C))

# Conditionally add mesh + net_server when libsodium is available
OPTIONAL_SRCS =
ifeq ($(SODIUM_FOUND),yes)
OPTIONAL_SRCS += mesh.c mesh_identity.c
OPTIONAL_SRCS += net_tool.c fleet_bridge.c
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

# ── Hardened release switch (HARDEN=1) ──────────────────────────────────────
# Compile the anti-RE bodies in (-DDSCO_HARDENED), obfuscate sensitive string
# literals (-DDSCO_USE_OBF_SECRETS), drop debug info (-g) and the compiler ident
# string, and strip local/debug symbols at link. The `harden` target drives this
# into an isolated build dir, then strips + hardened-signs the result. Placed
# after all platform BASE_CFLAGS mutations so the filter-out sees the final set.
ifeq ($(HARDEN),1)
BASE_CFLAGS := $(filter-out -g,$(BASE_CFLAGS)) -DDSCO_HARDENED -DDSCO_USE_OBF_SECRETS -fno-ident
ifeq ($(UNAME_S),Darwin)
RELEASE_LDFLAGS += -Wl,-x -Wl,-S
else
RELEASE_LDFLAGS += -Wl,-x -Wl,-s -Wl,--build-id=none -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
endif
# Extra layer (HARDEN_CSTRING=1, driven by `make harden-max`, macOS only):
# relocate __TEXT,__cstring into the writable __DATA segment so a post-link tool
# can encrypt it and src/cstring_unlock.c can decrypt it in place at load.
ifeq ($(HARDEN_CSTRING),1)
BASE_CFLAGS += -DDSCO_HARDEN_CSTRING
RELEASE_LDFLAGS += -Wl,-rename_section,__TEXT,__cstring,__DATA,__cstring
endif
endif

all: $(TARGET) dsc dsco-new $(LITE_TARGET) $(SPINE_TARGET)
	@# Keep PATH current: every full build refreshes $(BINDIR) via `install`.
	@# Skip with DSCO_NO_INSTALL=1; CI never auto-installs.
	@if [ "$${DSCO_NO_INSTALL:-0}" = "1" ] || [ "$${DSCO_CI:-0}" = "1" ]; then \
		echo "skipping PATH install (DSCO_NO_INSTALL/CI)"; \
	else \
		$(MAKE) --no-print-directory install >/dev/null && \
		echo "PATH refreshed: installed dsco, dsco-lite, dsc, dsco-new to $(BINDIR)"; \
	fi
debug: $(DEBUG_TARGET)
dev: $(DEBUG_TARGET)

# ── Hardened, ship-ready binary ─────────────────────────────────────────────
# Builds the anti-RE profile into an isolated obj dir (keeps dev objects warm),
# strips all local/debug symbols, and applies a hardened-runtime code signature
# (blocks debugger attach + dyld injection for non-root). No .dbg is emitted.
# Override the signing identity: `make harden DSCO_CODESIGN_ID="Developer ID..."`.
DSCO_CODESIGN_ID ?= -
.PHONY: harden harden-verify
harden:
	@echo "── building hardened dsco (anti-RE) ──"
	rm -f $(TARGET)
	$(MAKE) HARDEN=1 BUILD_DIR=build-harden $(TARGET)
	strip -x $(TARGET) 2>/dev/null || strip $(TARGET)
ifeq ($(UNAME_S),Darwin)
	@codesign --remove-signature $(TARGET) 2>/dev/null || true
	codesign --force --options runtime --entitlements scripts/harden.entitlements --sign "$(DSCO_CODESIGN_ID)" $(TARGET)
endif
	@$(MAKE) --no-print-directory harden-verify

harden-verify:
	@echo "── hardened build report ──"
	@ls -la $(TARGET)
	@printf 'symbols:      '; nm $(TARGET) 2>/dev/null | wc -l | tr -d ' '
	@printf 'local syms:   '; nm $(TARGET) 2>/dev/null | grep -cE ' [tdb] ' || echo 0
ifeq ($(UNAME_S),Darwin)
	@printf 'codesign:     '; codesign -dv $(TARGET) 2>&1 | grep -iE 'flags' || echo '(unsigned)'
endif
	@printf 'string leak:  '; strings $(TARGET) 2>/dev/null | grep -cE '"env":|"comment":|api\.anthropic|oauth' | sed 's/$$/ sensitive strings (lower is better)/'
	@printf 'total strings: '; strings $(TARGET) 2>/dev/null | wc -l | tr -d ' '

# ── Maximum hardening: everything in `harden` PLUS __cstring encryption ──────
# The C string-literal pool is relocated to a writable segment, encrypted at
# rest post-link, and decrypted at load by a constructor. This is what drops the
# raw `strings`/Ghidra count from tens of thousands to near-zero. macOS only.
.PHONY: harden-max
harden-max:
	@echo "── building MAX-hardened dsco (anti-RE + __cstring encryption) ──"
	rm -f $(TARGET)
	python3 scripts/gen_cstring_key.py include/cstring_key.gen.h build/.cstring_key
	$(MAKE) HARDEN=1 HARDEN_CSTRING=1 BUILD_DIR=build-hardenmax $(TARGET)
	strip -x $(TARGET) 2>/dev/null || strip $(TARGET)
	python3 scripts/encrypt_cstring.py $(TARGET) build/.cstring_key
ifeq ($(UNAME_S),Darwin)
	@codesign --remove-signature $(TARGET) 2>/dev/null || true
	codesign --force --options runtime --entitlements scripts/harden.entitlements --sign "$(DSCO_CODESIGN_ID)" $(TARGET)
endif
	@$(MAKE) --no-print-directory harden-verify

profile-instrumented:
	$(MAKE) BUILD_DIR=build/instrumented TARGET=$(PROFILE_TARGET) PROFILE_BUILD=1 $(PROFILE_TARGET)

profile:
	python3 scripts/dsco_profile.py -- ./$(PROFILE_TARGET) --version

.PHONY: wasm wasm-check wasm-smoke-native test_wasm_core
wasm: $(WASM_TARGET)

wasm-check:
	@command -v emcc >/dev/null 2>&1 || { \
		echo "emcc not found; install Emscripten to build $(WASM_TARGET)"; \
		exit 1; \
	}

$(WASM_TARGET): $(SRC_DIR)/wasm_core.c $(INC_DIR)/wasm_core.h $(INC_DIR)/config.h | wasm-check $(BUILD_DIR)
	EM_CACHE=$(WASM_CACHE_ABS) emcc -Oz -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -I$(INC_DIR) \
		-DBUILD_DATE='"$(BUILD_DATE)"' -DGIT_HASH='"$(GIT_HASH)"' \
		--no-entry \
		-sMODULARIZE=1 -sEXPORT_NAME=DscoWasm -sENVIRONMENT=web,node \
		-sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 \
		-sEXPORTED_RUNTIME_METHODS='["ccall","cwrap"]' \
		-sEXPORTED_FUNCTIONS=$(WASM_EXPORTS) \
		-o $@ $<

wasm-smoke-native: test_wasm_core

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
	rm -rf build/cosmo \
		$(COSMO_TARGET) $(COSMO_TARGET).dbg $(COSMO_TARGET).com.dbg $(COSMO_TARGET).aarch64.elf \
		$(COSMO_LEGACY_TARGET) $(COSMO_LEGACY_TARGET).dbg $(COSMO_LEGACY_TARGET).aarch64.elf

cosmo-info:
	@echo "COSMO_TARGET=$(COSMO_TARGET)"
	@echo "COSMO_LEGACY_TARGET=$(COSMO_LEGACY_TARGET)"
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
	@if command -v compiledb >/dev/null 2>&1; then \
		echo "compiledb: capturing real build flags -> compile_commands.json"; \
		compiledb -n make -B; \
	else \
		echo "gen: compiledb not found; using dependency-light generator"; \
		python3 scripts/gen_compile_commands.py; \
	fi
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

dsc: demos/toys/dsc.c
	$(CC) -O2 -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -o $@ $< -lcurl -lreadline

# Standalone Mobius logo exploration; deliberately separate from canonical branding.
dsco-mobius: $(SRC_DIR)/mobius_main.c $(SRC_DIR)/mobius.c $(SRC_DIR)/kitty_graphics.c include/mobius.h include/kitty_graphics.h
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/mobius_main.c $(SRC_DIR)/mobius.c $(SRC_DIR)/kitty_graphics.c -lm -lz

# Standalone animated Distributed Systems wordmark (Kitty graphics protocol).
dsco-banner: $(SRC_DIR)/kitty_banner_main.c $(SRC_DIR)/kitty_banner.c \
		$(SRC_DIR)/kitty_graphics.c $(SRC_DIR)/px_theme.c $(INC_DIR)/kitty_banner.h \
		$(INC_DIR)/kitty_banner_mask.h $(INC_DIR)/kitty_graphics.h $(INC_DIR)/px_theme.h
	$(CC) -O2 -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -I$(INC_DIR) \
		-o $@ $(SRC_DIR)/kitty_banner_main.c $(SRC_DIR)/kitty_banner.c \
		$(SRC_DIR)/kitty_graphics.c $(SRC_DIR)/px_theme.c -lz -lm

# Native semantic surface gallery. Use `--ppm /tmp/dsco-lab.ppm` headlessly,
# or run `./dsco-kitty-lab --animate` inside Kitty/Ghostty/WezTerm.
dsco-kitty-lab: $(SRC_DIR)/kitty_lab_main.c $(SRC_DIR)/kitty_lab.c \
		$(SRC_DIR)/kitty_graphics.c $(SRC_DIR)/pixel_hdr.c \
		$(INC_DIR)/kitty_lab.h $(INC_DIR)/kitty_graphics.h $(INC_DIR)/pixel_hdr.h
	$(CC) -O2 -std=$(DSCO_STD) $(C2Y_WARNING_FLAGS) -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -I$(INC_DIR) \
		-o $@ $(SRC_DIR)/kitty_lab_main.c $(SRC_DIR)/kitty_lab.c \
		$(SRC_DIR)/kitty_graphics.c $(SRC_DIR)/pixel_hdr.c -lz -lm

$(DEBUG_TARGET): $(DEBUG_OBJS) $(GSL_DEBUG_OBJS)
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Publish only a fully linked and signed fresh inode, never patch a live one.
$(TARGET): $(OBJS) $(GSL_OBJS)
	@set -eu; tmp=$$(mktemp "$@.tmp.XXXXXX"); \
	trap 'rm -f "$$tmp"' EXIT HUP INT TERM; \
	$(CC) $(CFLAGS) -o "$$tmp" $^ $(LDFLAGS) $(RELEASE_LDFLAGS) $(LDLIBS); \
	chmod 755 "$$tmp"; \
	if [ "$(UNAME_S)" = Darwin ]; then \
		codesign --force --sign - "$$tmp"; \
		codesign --verify --strict "$$tmp"; \
	fi; \
	mv -f "$$tmp" "$@"

# dsco-new is a twin of dsco — same code, same composer, distinct name.
dsco-new: $(TARGET)
	sh scripts/install_atomic.sh "$(TARGET)" "$@"

$(LITE_TARGET): $(SRC_DIR)/lite_main.c $(INC_DIR)/config.h
	$(CC) $(LITE_CFLAGS) -o $@ $<
	-strip -x $@ 2>/dev/null || true

$(SPINE_TARGET): $(SRC_DIR)/spine_dsco_slim.c $(LIB_OBJS) $(GSL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(RELEASE_LDFLAGS) $(LDLIBS)

.PHONY: test-spine-dsco-slim
test-spine-dsco-slim: $(SPINE_TARGET)
	sh $(TEST_DIR)/test_spine_dsco_slim.sh ./$(SPINE_TARGET)

# Source compilation rules
# ── Pizza box: bake data/ blobs before generated .o files are compiled ──
.PHONY: test-bake-dependencies
test-bake-dependencies:
	python3 tests/test_bake_dependencies.py

.PHONY: bake_data
bake_data: $(BUILD_DIR)/.bake_data.stamp

$(BUILD_DIR)/.bake_data.stamp: $(BAKED_DATA) scripts/bake_data.sh scripts/bake_data.py | $(BUILD_DIR)
	@bash scripts/bake_data.sh data src/generated include
	@touch $@

$(GENERATED_C) $(GENERATED_REGISTRY) include/embedded_key.gen.h: $(BUILD_DIR)/.bake_data.stamp
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

$(TSAN_TEST_OBJ_DIR)/generated_%.o: src/generated/%.c | bake_data $(TSAN_TEST_OBJ_DIR)
	$(CC) $(TSAN_CFLAGS) -c -o $@ $<

$(ASAN_UBSAN_TEST_OBJ_DIR)/generated_%.o: src/generated/%.c | bake_data $(ASAN_UBSAN_TEST_OBJ_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

include/lingo_runtime.gen.h: lingo/runtime.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@

include/lingo_operator.gen.h: lingo/operator.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_operator

include/lingo_dsco.gen.h: lingo/dsco.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_dsco

include/lingo_autobot.gen.h: lingo/autobot.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_autobot

include/lingo_chimera.gen.h: lingo/chimera.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_chimera

include/lingo_world_io.gen.h: lingo/world_io.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_world_io

include/lingo_platform.gen.h: lingo/platform.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_platform

include/lingo_workspace.gen.h: lingo/workspace.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_workspace

include/lingo_view.gen.h: lingo/view.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_view

include/lingo_workflow.gen.h: lingo/workflow.lua scripts/embed_lingo.py
	python3 scripts/embed_lingo.py $< $@ lingo_workflow

LINGO_OBJS = $(foreach d,$(OBJ_DIR) $(DEBUG_OBJ_DIR) $(TEST_OBJ_DIR) $(TEST_COVERAGE_OBJ_DIR) $(ASAN_OBJ_DIR) $(UBSAN_OBJ_DIR) $(ASAN_TEST_OBJ_DIR) $(UBSAN_TEST_OBJ_DIR) $(TSAN_TEST_OBJ_DIR) $(ASAN_UBSAN_TEST_OBJ_DIR),$(d)/lingo.o)
LINGO_ORIGIN_OBJS = $(foreach d,$(OBJ_DIR) $(DEBUG_OBJ_DIR) $(TEST_OBJ_DIR) $(TEST_COVERAGE_OBJ_DIR) $(ASAN_OBJ_DIR) $(UBSAN_OBJ_DIR) $(ASAN_TEST_OBJ_DIR) $(UBSAN_TEST_OBJ_DIR) $(TSAN_TEST_OBJ_DIR) $(ASAN_UBSAN_TEST_OBJ_DIR),$(d)/lingo_origin.o)
$(LINGO_OBJS): include/lingo_runtime.gen.h include/lingo_operator.gen.h include/lingo_dsco.gen.h include/lingo_autobot.gen.h include/lingo_chimera.gen.h include/lingo_world_io.gen.h include/lingo_platform.gen.h include/lingo_workspace.gen.h include/lingo_view.gen.h include/lingo_workflow.gen.h
# LuaJIT external unwinding cannot traverse PAC-signed C return addresses on
# this macOS arm64 runtime. Keep BTI; scope the exception to Lua host callbacks.
ifeq ($(UNAME_S)-$(UNAME_M),Darwin-arm64)
ifneq ($(COSMO_BUILD),1)
$(LINGO_OBJS) $(LINGO_ORIGIN_OBJS): override BASE_CFLAGS += -mbranch-protection=bti
endif
endif

.PHONY: test-lingo test-lingo-operator test-lingo-systems test-lingo-world test-lingo-platform test-lingo-workspace test-lingo-session
test-lingo: dsco
	python3 tests/test_lingo.py ./dsco

.PHONY: test-lingo-ipc test-event-stream test-lingo-event-semantics test-provider-event-stream
test-lingo-ipc: dsco
	python3 tests/test_lingo_ipc.py ./dsco

test-provider-event-stream: dsco $(BUILD_DIR)/stream_completion_fixture
	python3 tests/test_provider_event_stream.py --dsco ./dsco --fixture $(BUILD_DIR)/stream_completion_fixture --output $(BUILD_DIR)/provider-event-capture.json

test-lingo-event-semantics:
	python3 tests/test_lingo_event_semantics.py

test-event-stream:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude -o $(BUILD_DIR)/test_event_stream tests/test_event_stream.c src/event_stream.c vendor/yyjson.c -lsqlite3 -lpthread
	python3 tests/run_event_stream_tests.py $(BUILD_DIR)/test_event_stream

test-lingo-world: dsco
	python3 tests/test_lingo_world.py ./dsco

test-lingo-platform: dsco
	python3 tests/test_lingo_platform.py ./dsco

test-lingo-workspace: dsco
	python3 tests/test_lingo_workspace.py ./dsco

test-lingo-session: dsco
	python3 tests/test_lingo_session.py ./dsco

.PHONY: test-lingo-workflow
test-lingo-workflow: dsco
	python3 tests/test_lingo_workflow.py ./dsco

test-lingo-operator: dsco
	python3 tests/test_lingo_operator.py ./dsco

test-lingo-systems: dsco
	python3 tests/test_lingo_systems.py ./dsco

# embedded_data.c and tools.c pull in the generated registry.
$(OBJ_DIR)/embedded_data.o: include/embedded_data_registry.h include/embedded_key.gen.h
$(OBJ_DIR)/tools.o: include/embedded_data_registry.h
$(DEBUG_OBJ_DIR)/embedded_data.o: include/embedded_data_registry.h include/embedded_key.gen.h
$(TEST_OBJ_DIR)/embedded_data.o: include/embedded_data_registry.h include/embedded_key.gen.h

# The __cstring decryptor runs at constructor priority 101 — before libSystem
# initializes __stack_chk_guard — so it must be stack-protector-free (a canary
# check against an uninitialized guard SIGTRAPs). It is self-contained (inlined
# SHA-256), so -fno-builtin/-fno-stack-protector are safe and have no other cost.
$(OBJ_DIR)/cstring_unlock.o: CFLAGS := $(filter-out -fstack-protector-strong -D_FORTIFY_SOURCE=2,$(CFLAGS)) -fno-stack-protector -fno-builtin
# Force recompile whenever the per-build key header changes (only exists under HARDEN_CSTRING).
ifeq ($(HARDEN_CSTRING),1)
$(OBJ_DIR)/cstring_unlock.o: include/cstring_key.gen.h
endif

ifeq ($(PROFILE_BUILD),1)
$(OBJ_DIR)/instrumenter.o: CFLAGS := $(filter-out $(PROFILE_COVERAGE_FLAGS),$(CFLAGS)) -fsanitize-coverage=0
endif

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

$(ASAN_TEST_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(ASAN_TEST_OBJ_DIR)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

$(UBSAN_TEST_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(UBSAN_TEST_OBJ_DIR)
	$(CC) $(UBSAN_CFLAGS) -c -o $@ $<

$(TSAN_TEST_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(TSAN_TEST_OBJ_DIR)
	$(CC) $(TSAN_CFLAGS) -c -o $@ $<

$(ASAN_UBSAN_TEST_OBJ_DIR)/gsl_%.o: gsl/src/%.c | $(ASAN_UBSAN_TEST_OBJ_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -c -o $@ $<

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

$(TSAN_TEST_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(TSAN_TEST_OBJ_DIR)
	$(CC) $(TSAN_CFLAGS) -c -o $@ $<

$(TSAN_TEST_OBJ_DIR)/test_%.o: $(TEST_DIR)/test_%.c | $(TSAN_TEST_OBJ_DIR)
	$(CC) $(TSAN_CFLAGS) -c -o $@ $<

$(TSAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(TSAN_TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) -c -o $@ $<

$(TSAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(TSAN_TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

$(ASAN_UBSAN_TEST_OBJ_DIR)/test.o: $(TEST_DIR)/test.c | $(ASAN_UBSAN_TEST_OBJ_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -c -o $@ $<

$(ASAN_UBSAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(ASAN_UBSAN_TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(ASAN_UBSAN_CFLAGS) -c -o $@ $<

$(ASAN_UBSAN_TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.m | $(ASAN_UBSAN_TEST_OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(ASAN_UBSAN_CFLAGS) -fobjc-arc -x objective-c -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

$(OBJ_DIR) $(DEBUG_OBJ_DIR) $(TEST_OBJ_DIR) $(TEST_COVERAGE_OBJ_DIR) $(ASAN_OBJ_DIR) $(UBSAN_OBJ_DIR) $(ASAN_TEST_OBJ_DIR) $(UBSAN_TEST_OBJ_DIR) $(TSAN_TEST_OBJ_DIR) $(ASAN_UBSAN_TEST_OBJ_DIR):
	mkdir -p $@
	mkdir -p $@/extension

# Header dependency tracking: -MMD -MP (in BASE_CFLAGS) emits a .d file next to
# each .o listing the headers it included. Including them here makes any object
# rebuild when a header it uses changes — e.g. editing include/config.h now
# correctly recompiles every .o that includes it, instead of silently shipping
# a stale binary.
-include $(wildcard $(BUILD_DIR)/*/*.d)

# test_runner's dsco-subgoal integration case fork/execs ./dsco; build the
# shipped binary first so `make test` never relies on a stale local artifact.
test: $(TARGET) test_runner test-cap-flow-race test-execution-kernel test-execution-spine-mcp test-goal-queue test-dsco-meta test-config-surface test-work-algebra test-request-intent test-goal-controller-binary test-tool-assurance test-headless-tool-guard test-headless-tool-guard-binary test-agent-interop test-execution-contract
	./test_runner

.PHONY: test-headless-tool-guard-binary
test-headless-tool-guard-binary: $(TARGET)
	python3 tests/test_headless_tool_guard_binary.py --binary ./$(TARGET)

.PHONY: test-headless-tool-guard
test-headless-tool-guard:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_headless_tool_guard.c src/headless_tool_guard.c src/tool_assurance.c
	$(BUILD_DIR)/$@

.PHONY: test-execution-contract
test-execution-contract:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_execution_contract.c src/execution_contract.c src/json_util.c src/env_config.c
	$(BUILD_DIR)/$@

.PHONY: test-tool-assurance
test-tool-assurance:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_tool_assurance.c src/tool_assurance.c
	$(BUILD_DIR)/$@

.PHONY: test-agent-interop
test-agent-interop: $(TARGET)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_agent_interop.c src/agent_interop.c src/agent_run_outcome.c src/subscription_paths.c src/process_capture.c src/json_util.c vendor/yyjson.c
	$(BUILD_DIR)/$@
	python3 tests/test_agent_interop_binary.py --binary ./$(TARGET)
	python3 tests/test_subscription_paths_binary.py --binary ./$(TARGET)
	python3 tests/test_agent_interop_conformance.py
	python3 tests/test_agent_interop_protocols.py --binary ./$(TARGET)
	python3 tests/test_acp_server.py --binary ./$(TARGET)
	python3 tests/test_swarm_interop_mcp.py --binary ./$(TARGET)

.PHONY: test-agent-interop-live test-agent-interop-live-invoke
test-agent-interop-live: $(TARGET)
	python3 scripts/agent_interop_conformance.py --binary ./$(TARGET) --require-all

test-agent-interop-live-invoke: $(TARGET)
	python3 scripts/agent_interop_conformance.py --binary ./$(TARGET) --require-all --invoke

# End-to-end behavioral verification of documented gate claims against the
# LIVE binary (drives `dsco mcp serve` over JSON-RPC; no LLM, deterministic).
# NOTE: this verifies the gate itself — it runs with DSCO_GOV_BYPASS unset so
# a shell-level DSCO_GOV_BYPASS=1 / DSCO_GOV_MODEL=none cannot silently make
# these checks pass against an ungoverned process.
.PHONY: test-blackboard
test-blackboard: $(TARGET)
	python3 tests/test_blackboard.py --binary ./$(TARGET)
	python3 tests/test_ipc_task_fencing.py
	python3 tests/test_ipc_target_recovery.py
	python3 tests/test_durable_boot_fencing.py --binary ./$(TARGET)

test-gate-claims: $(TARGET) test-cap-flow-race test-openrouter-lane-identity
	env -u DSCO_GOV_BYPASS -u DSCO_GOV_MODEL \
		-u DSCO_ALLOW_READ -u DSCO_ALLOW_WRITE -u DSCO_ALLOW_NET \
		-u DSCO_ALLOW_RUN -u DSCO_ALLOW_SECRETS -u DSCO_ALLOW_CONTROL \
		-u DSCO_ALLOW_EXFIL \
		bash tests/verify_gate_claims.sh ./dsco

.PHONY: test-chronicle-headless-binary
test-chronicle-headless-binary: $(TARGET)
	python3 tests/test_chronicle_headless_binary.py ./$(TARGET)

.PHONY: test-interactive-cost-failure-chronicle
test-interactive-cost-failure-chronicle: $(TARGET)
	python3 tests/test_interactive_cost_failure_chronicle.py ./$(TARGET)

.PHONY: test-spine-chronicle
test-spine-chronicle: $(BUILD_DIR)/spine_chronicle_fixture
	python3 tests/test_spine_chronicle.py ./$(BUILD_DIR)/spine_chronicle_fixture

$(BUILD_DIR)/spine_chronicle_fixture: tests/spine_chronicle_fixture.c $(LIB_OBJS) $(GSL_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ $^ $(LDFLAGS) $(LDLIBS)

.PHONY: test-orchestrator-chronicle
test-orchestrator-chronicle: $(BUILD_DIR)/orchestrator_chronicle_fixture
	python3 tests/test_orchestrator_chronicle.py ./$(BUILD_DIR)/orchestrator_chronicle_fixture

$(BUILD_DIR)/orchestrator_chronicle_fixture: tests/orchestrator_chronicle_fixture.c $(LIB_OBJS) $(GSL_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ $^ $(LDFLAGS) $(LDLIBS)

.PHONY: test-realtime-chronicle
test-realtime-chronicle: $(BUILD_DIR)/realtime_chronicle_fixture
	python3 tests/test_realtime_chronicle.py ./$(BUILD_DIR)/realtime_chronicle_fixture

.PHONY: test-voice-chronicle-capture
test-voice-chronicle-capture: $(BUILD_DIR)/voice_session_fixture
	python3 tests/test_voice_chronicle_capture.py ./$(BUILD_DIR)/voice_session_fixture

$(BUILD_DIR)/realtime_chronicle_fixture: tests/realtime_chronicle_fixture.c $(LIB_OBJS) $(GSL_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ $< $(filter-out $(OBJ_DIR)/realtime.o,$(LIB_OBJS)) $(GSL_OBJS) $(LDFLAGS) $(LDLIBS)

.PHONY: test-prompt-branches
test-prompt-branches: $(TARGET)
	python3 tests/test_prompt_branches.py --binary "$(abspath $(TARGET))"

.PHONY: test-directive-store
test-directive-store:
	$(CC) $(TEST_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_directive_store tests/test_directive_store.c src/directive_store.c src/workspace.c src/capsule.c tests/directive_context_stubs.c src/json_fast.c src/crypto.c src/json_util.c $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/test_directive_store

.PHONY: test-workspace-skills
test-workspace-skills:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_workspace_skills tests/test_workspace_skills.c src/workspace.c src/capsule.c tests/directive_context_stubs.c src/json_fast.c src/crypto.c src/json_util.c $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/test_workspace_skills

.PHONY: test-typed-boundaries
# Offline codec/adapter contracts, including deterministic I/O failure injection.
test-typed-boundaries:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -DDSCO_INTERNAL_TESTS -o $(BUILD_DIR)/test_turn_record tests/test_turn_record.c src/turn_record.c src/json_util.c $(ASAN_LDFLAGS)
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_turn_record
	$(CC) $(ASAN_CFLAGS) -o $(BUILD_DIR)/test_task_profile_tool tests/test_task_profile_tool.c src/task_profile_tool.c src/task_profile.c src/json_fast.c src/json_util.c $(ASAN_LDFLAGS)
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_task_profile_tool

.PHONY: test-json-skip bench-json-skip
# Offline hot-path regression/benchmark; does not invoke providers or tools.
test-json-skip:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -o $(BUILD_DIR)/test_json_skip tests/test_json_skip.c src/json_util.c $(ASAN_LDFLAGS)
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_json_skip

bench-json-skip:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/bench_json_skip bench/bench_json_skip.c src/json_util.c
	$(BUILD_DIR)/bench_json_skip

.PHONY: test-json-scan-bounds
# Standalone decoder bounds regression; no providers, network, or runtime state.
test-json-scan-bounds:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -o $(BUILD_DIR)/test_json_scan_bounds tests/test_json_scan_bounds.c src/json_util.c $(ASAN_LDFLAGS)
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_json_scan_bounds

.PHONY: test-value-ledger
test-value-ledger: $(TARGET)
	$(CC) $(TEST_CFLAGS) -Iinclude -c tests/test_value_ledger.c -o $(BUILD_DIR)/obj/test_value_ledger.o
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/test_value_ledger \
		$(BUILD_DIR)/obj/test_value_ledger.o \
		$(filter-out $(BUILD_DIR)/obj/main.o $(BUILD_DIR)/obj/mcp_server.o,$(OBJS)) $(GSL_OBJS) \
		$(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/test_value_ledger

test-fast: $(TARGET) test_runner test_command_plane
	./test_command_plane
	DSCO_TEST_QUICK=1 ./test_runner

# Diff the Claude Code OAuth wire fingerprint (version/beta list/entrypoint/
# User-Agent literals) against upstream oh-my-pi. Anthropic ships new Claude
# Code releases on no fixed schedule and each one can silently invalidate
# these bytes; run this after pulling oh-my-pi to catch drift before it ships.
.PHONY: check-oauth-fingerprint
check-oauth-fingerprint:
	python3 scripts/check_claude_oauth_fingerprint.py

# Client SDK: unit tests (framing/correlation, no binary needed) + live e2e vs ./dsco
test-sdk: $(TARGET)
	cd npm/dsco-sdk && node --test test/unit.test.mjs && DSCO_BIN=$(CURDIR)/dsco node --test test/e2e.test.mjs

test-cli-flags: $(TARGET)
	bash tests/test_cli_global_flags.sh ./$(TARGET)

.PHONY: test-native-cli-routing
test-native-cli-routing: $(TARGET)
	python3 tests/test_native_cli_routing.py ./$(TARGET)

.PHONY: test-native-storage
test-native-storage:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_native_storage tests/test_native_storage.c src/crypto.c $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/test_native_storage

# Deterministic capability-gate hardening test (G04 .git control-writes,
# G05 symlink-scope escape). Links only the gate + json objects plus a tiny
# stub for the registry read-only predicate. No network, no LLM.
test-cap-hardening: $(TARGET)
	printf '#include <stdbool.h>\nbool tools_meta_is_read_only(const char *n,bool *found){(void)n;*found=false;return false;}\nbool buffer_store_sensitive(const char *n){(void)n;return false;}\nbool native_windows_sensitive(void){return false;}\n' > build/obj/_cap_test_stub.c
	$(CC) $(CFLAGS) -o test_cap_hardening tests/test_capability_hardening.c \
		build/obj/capability.o build/obj/surface_policy.o build/obj/json_util.o build/obj/json_fast.o \
		build/obj/_cap_test_stub.c
	./test_cap_hardening

.PHONY: test-cap-flow-race
test-cap-flow-race:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude -ffunction-sections -fdata-sections \
		-o $(BUILD_DIR)/test_capability_flow_race tests/test_capability_flow_race.c \
		src/capability.c src/surface_policy.c src/json_util.c src/json_fast.c \
		$(RELEASE_LDFLAGS) -lpthread
	$(BUILD_DIR)/test_capability_flow_race

.PHONY: test-openrouter-lane-identity
test-openrouter-lane-identity:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude \
		$$(pkg-config --cflags libcurl) -o $(BUILD_DIR)/test_openrouter_lane_identity \
		tests/test_openrouter_lane_identity.c src/json_util.c src/json_fast.c \
		$$(pkg-config --libs libcurl)
	$(BUILD_DIR)/test_openrouter_lane_identity

# Deterministic HDR compositing test: tonemap curve invariants, sRGB identity
# round-trip, bloom propagation vs a bloom-off control, dither determinism and
# banding, allocation guards. Pure CPU, no terminal, no network, no LLM.
test-pixel-hdr:
	$(CC) $(TEST_CFLAGS) -Iinclude -o test_pixel_hdr tests/test_pixel_hdr.c src/pixel_hdr.c -lm
	./test_pixel_hdr

# Deterministic cognitive-orchestration kernel test. No network or LLM.
test-overmind:
	$(CC) $(TEST_CFLAGS) -o test_overmind tests/test_overmind.c src/overmind.c
	./test_overmind

test_runner: $(TEST_OBJS) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

.PHONY: test-pixel-hdr asan-test leak-test ubsan-test asan-ubsan-test tsan-test sanitizer-test

test_runner_tsan: $(TSAN_TEST_OBJS) $(GSL_TSAN_TEST_OBJS)
	$(CC) $(TSAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(TSAN_LDFLAGS) $(LDLIBS)

test_runner_asan: $(ASAN_TEST_OBJS) $(GSL_ASAN_TEST_OBJS)
	$(CC) $(ASAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(ASAN_LDFLAGS) $(LDLIBS)

test_runner_ubsan: $(UBSAN_TEST_OBJS) $(GSL_UBSAN_TEST_OBJS)
	$(CC) $(UBSAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(UBSAN_LDFLAGS) $(LDLIBS)

test_runner_asan_ubsan: $(ASAN_UBSAN_TEST_OBJS) $(GSL_ASAN_UBSAN_TEST_OBJS)
	$(CC) $(ASAN_UBSAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(ASAN_UBSAN_LDFLAGS) $(LDLIBS)

asan-test: test_runner_asan
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1:abort_on_error=1' ./test_runner_asan

# Dedicated leak lane. On runtimes that do not implement LeakSanitizer this
# target fails rather than silently certifying the tree; macOS also has the
# native `leaks-test` smoke lane below.
leak-test: test_runner_asan
	ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=1' ./test_runner_asan

ubsan-test: test_runner_ubsan
	UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' ./test_runner_ubsan

asan-ubsan-test: test_runner_asan_ubsan
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1:abort_on_error=1' \
	UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' ./test_runner_asan_ubsan

tsan-test: test_runner_tsan test_plan_cache_tsan
	TSAN_OPTIONS='halt_on_error=1:second_deadlock_stack=1' ./test_runner_tsan
	TSAN_OPTIONS='halt_on_error=1:second_deadlock_stack=1' ./test_plan_cache_tsan

sanitizer-test: asan-ubsan-test tsan-test

model-resolution-sim: $(TARGET)
	python3 scripts/model_resolution_sim.py --dsco ./$(TARGET)

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

test_tui_snapshots: test_tui_snapshot test_tui_theme_snapshot test_pixel_plan

# Pixel compositor geometry/DPR tests (headless; public native_ui API only)
.PHONY: test_pixel_geometry test_native_compositor
test_pixel_geometry: $(TEST_OBJ_DIR)/test_pixel_geometry.o $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

# Full retained-compositor, performance telemetry, transport, and parity suite.
test_native_compositor: $(TEST_OBJ_DIR)/test_native_compositor.o $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

# Headless native plan tree/action-DAG rendering artifacts.
.PHONY: test_pixel_plan
test_pixel_plan: $(TEST_OBJ_DIR)/test_pixel_plan.o $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

# Headless Kitty APC framing, query, and terminal-hint contract tests.
.PHONY: test_tui_swarm_dock test_tui_swarm_composer
test_tui_swarm_composer: tui_swarm_composer_fixture
	python3 tests/test_tui_swarm_composer.py

tui_swarm_composer_fixture: tests/tui_swarm_composer_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(LDLIBS)

test_tui_swarm_dock: tests/test_tui_swarm_dock.c src/tui_swarm_dock.c include/tui_swarm_dock.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -DDSCO_TUI_SWARM_DOCK_TEST -o $(BUILD_DIR)/$@ tests/test_tui_swarm_dock.c src/tui_swarm_dock.c -lpthread
	$(BUILD_DIR)/$@

.PHONY: test_kitty_agent_windows
test_kitty_agent_windows: tests/test_kitty_agent_windows.c src/kitty_agent_windows.c include/kitty_agent_windows.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_kitty_agent_windows.c
	$(BUILD_DIR)/$@

.PHONY: test_pty_session test_desktop_adapter test_surface_transport test_surface_cli test_dynamic_tool_cache test_browser_session test_harness_surfaces test_surface_workspace
test_surface_cli:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_surface_cli.c src/surface_cli.c src/json_fast.c
	$(BUILD_DIR)/$@

.PHONY: test_context_eviction test_task_closeout
test_context_eviction:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/$@ tests/test_context_eviction.c src/context_eviction.c src/context_archive.c src/llm.c src/tool_integrity_index.c src/json_util.c src/json_fast.c -lcurl -lm
	$(BUILD_DIR)/$@

.PHONY: test-agent-consensus
test-agent-consensus:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_agent_consensus tests/test_agent_consensus.c src/agent_consensus.c -lm
	$(BUILD_DIR)/test_agent_consensus

.PHONY: test-jev-router
test-jev-router:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_jev_router tests/test_jev_router.c src/jev_router.c src/router.c src/crypto.c src/json_util.c src/json_fast.c -lm -lpthread
	$(BUILD_DIR)/test_jev_router

.PHONY: test-jev-compaction
test-jev-compaction:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_jev_compaction tests/test_jev_compaction.c src/jev_compaction.c src/json_util.c src/json_fast.c -lm
	@for scenario in success denied malformed missing range bound; do $(BUILD_DIR)/test_jev_compaction $$scenario || exit $$?; done

.PHONY: test-context-recovery
test-context-recovery: $(TARGET) test_context_eviction
	python3 tests/test_context_proxy_tools.py
	python3 tests/test_context_recovery_binary.py --binary $(abspath $(TARGET))
	python3 tests/test_context_prose_binary.py ./$(TARGET)

test_task_closeout:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/$@ tests/test_task_closeout.c tests/context_store_fixture.c src/task_closeout.c src/context_eviction.c src/context_archive.c src/llm.c src/tool_integrity_index.c src/json_util.c src/json_fast.c -lcurl -lm
	$(BUILD_DIR)/$@
	python3 tests/test_next_action_dataset.py

.PHONY: test-result-spill-verified
test-result-spill-verified:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/test_result_spill_verified tests/test_result_spill_verified.c src/result_spill.c src/context_archive.c src/llm.c src/tool_integrity_index.c src/json_util.c src/json_fast.c -lcurl -lm
	$(BUILD_DIR)/test_result_spill_verified

.PHONY: test-context-archive-roundtrip
test-context-archive-roundtrip:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/test_context_archive_roundtrip tests/test_context_archive_roundtrip.c src/context_archive.c src/goal.c src/goal_queue.c src/llm.c src/tool_integrity_index.c src/json_util.c src/json_fast.c -lcurl -lm
	$(BUILD_DIR)/test_context_archive_roundtrip

.PHONY: test-request-context-progress
test-request-context-progress:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/test_request_context_progress tests/test_request_context_progress.c src/context_eviction.c src/context_archive.c src/input_budget.c src/compact_policy.c src/env_config.c src/error.c src/llm.c src/tool_integrity_index.c src/json_util.c src/json_fast.c -lcurl -lm
	$(BUILD_DIR)/test_request_context_progress

.PHONY: test-context-compaction
test-context-compaction: $(TARGET) test-context-lifecycle test_context_eviction test-request-context-progress test-context-archive-roundtrip test-result-spill-verified test-input-budget
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/test_context_compaction tests/test_context_compaction.c src/context_compaction.c src/task_closeout.c src/llm.c src/tool_integrity_index.c src/json_util.c src/json_fast.c -lcurl -lm
	$(BUILD_DIR)/test_context_compaction
	python3 tests/test_manual_compaction_binary.py --binary $(abspath $(TARGET)) --output reports/compaction-completion-20261003/repository-proof
	python3 tests/test_manual_compaction_binary.py --binary $(abspath $(TARGET)) --restart --omit-usage --output reports/compaction-completion-20261003/restart-proof
	python3 tests/test_manual_compaction_binary.py --binary $(abspath $(TARGET)) --batch 650 --restart --omit-usage --output reports/compaction-completion-20261003/batch-restart-proof
	python3 tests/test_manual_compaction_binary.py --binary $(abspath $(TARGET)) --write-denied --output reports/compaction-completion-20261003/write-denied-proof

.PHONY: test_app_control test_app_control_live
test_app_control:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -DDSCO_APP_CONTROL_NO_MACOS -DDSCO_VISION_GROUND_NO_NET -o $(BUILD_DIR)/$@ tests/test_app_control_unit.c src/app_control.c src/vision_ground.c src/crypto.c src/json_util.c src/json_fast.c -lm
	$(BUILD_DIR)/$@

# Live background control against an owned fixture; add ARGS=--vision to
# include TypeSafe grounding (sends only the synthetic fixture window).
test_app_control_live: $(TARGET)
	python3 tests/test_app_control_live.py --binary ./$(TARGET) $(ARGS)

# Drives the real pointer briefly against an owned fixture; restores it.
.PHONY: test_computer_live
test_computer_live: $(TARGET)
	python3 tests/test_computer_live.py --binary ./$(TARGET)

test_dynamic_tool_cache:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/$@ tests/test_dynamic_tool_cache.c src/llm.c src/tool_integrity_index.c src/tool_effects.c src/json_util.c vendor/yyjson.c -lcurl -lm
	$(BUILD_DIR)/$@

test_surface_workspace: $(TARGET)
	python3 tests/test_surface_workspace_mcp.py --binary ./$(TARGET)

.PHONY: test_desktop_live
test_desktop_live: $(TARGET)
	python3 tests/test_desktop_live_mcp.py --binary ./$(TARGET) --type-text 'Astra π 🦉'

test_pty_session:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -D_DARWIN_C_SOURCE -D_GNU_SOURCE -o $(BUILD_DIR)/$@ tests/test_pty_session.c src/pty_session.c src/json_util.c src/json_fast.c -lpthread $(if $(filter Linux,$(shell uname -s)),-lutil)
	$(BUILD_DIR)/$@

test_desktop_adapter:
	sh tests/verify_desktop_adapter.sh

test_surface_transport:
	sh tests/verify_surface_transport.sh

test_browser_session:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/browser_session_driver tests/browser_session_driver.c src/browser_session.c src/json_util.c src/json_fast.c -lpthread
	python3 tests/test_browser_session.py --binary $(BUILD_DIR)/browser_session_driver

test_harness_surfaces: $(TARGET)
	python3 tests/test_harness_surfaces_mcp.py --binary ./$(TARGET)

.PHONY: test_kitty_graphics test_kitty_patch_live test_tui_splash
test_tui_splash: $(TARGET)
	python3 tests/test_tui_splash.py --binary ./$(TARGET)

test_kitty_graphics: $(TEST_OBJ_DIR)/test_kitty_graphics.o \
	$(TEST_OBJ_DIR)/kitty_graphics.o
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_kitty_patch_live: tests/test_kitty_patch_live.c src/kitty_graphics.c include/kitty_graphics.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_kitty_patch_live.c src/kitty_graphics.c -lz
	$(BUILD_DIR)/$@

.PHONY: test_kitty_lab
test_kitty_lab: $(TEST_OBJ_DIR)/test_kitty_lab.o \
	$(TEST_OBJ_DIR)/kitty_lab.o $(TEST_OBJ_DIR)/kitty_graphics.o
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

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

test_plan_cache_tsan: $(TSAN_TEST_OBJ_DIR)/test_plan_cache.o \
	$(TSAN_TEST_OBJ_DIR)/plan_cache.o $(TSAN_TEST_OBJ_DIR)/json_util.o \
	$(TSAN_TEST_OBJ_DIR)/arena_alloc.o
	$(CC) $(TSAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(TSAN_LDFLAGS) -lm

$(TEST_OBJ_DIR)/test_learned_cost.o: $(TEST_DIR)/test_learned_cost.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_learned_cost: $(TEST_OBJ_DIR)/test_learned_cost.o \
	$(TEST_OBJ_DIR)/learned_cost.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

$(TEST_OBJ_DIR)/test_ooda_calibration.o: $(TEST_DIR)/test_ooda_calibration.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_ooda_calibration: $(TEST_OBJ_DIR)/test_ooda_calibration.o \
	$(TEST_OBJ_DIR)/ooda.o $(TEST_OBJ_DIR)/scheduler.o $(TEST_OBJ_DIR)/scheduler_lifetime.o $(TEST_OBJ_DIR)/scheduler_io.o $(TEST_OBJ_DIR)/event_loop.o $(TEST_OBJ_DIR)/event_wakeup.o $(TEST_OBJ_DIR)/error.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm
	./$@

$(TEST_OBJ_DIR)/test_session_memory.o: $(TEST_DIR)/test_session_memory.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
# session_memory pulls in vecstore + tools_embed_text; link full lib set.
test_session_memory: $(TEST_OBJ_DIR)/test_session_memory.o \
	$(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TEST_OBJ_DIR)/test_memory_keep_score.o: $(TEST_DIR)/test_memory_keep_score.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
# memory_keep_score pulls in vecstore + tools_embed_text via memory_tier.c; link full lib set.
test_memory_keep_score: $(TEST_OBJ_DIR)/test_memory_keep_score.o \
	$(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TEST_OBJ_DIR)/test_memory_classification.o: $(TEST_DIR)/test_memory_classification.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
# classification gates live in memory_tier.c; same full lib-set link as keep_score.
test_memory_classification: $(TEST_OBJ_DIR)/test_memory_classification.o \
	$(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TEST_OBJ_DIR)/test_command_plane.o: $(TEST_DIR)/test_command_plane.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_command_plane: $(TEST_OBJ_DIR)/test_command_plane.o $(TEST_OBJ_DIR)/command_plane.o $(TEST_OBJ_DIR)/error.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lsqlite3 -lm

$(TEST_OBJ_DIR)/test_net_fanout.o: $(TEST_DIR)/test_net_fanout.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_net_fanout: $(TEST_OBJ_DIR)/test_net_fanout.o \
	$(LIB_OBJS:$(OBJ_DIR)/%=$(TEST_OBJ_DIR)/%) $(GSL_TEST_OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TEST_OBJ_DIR)/test_wasm_core.o: $(TEST_DIR)/test_wasm_core.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
$(TEST_OBJ_DIR)/wasm_core.o: $(SRC_DIR)/wasm_core.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_wasm_core: $(TEST_OBJ_DIR)/test_wasm_core.o $(TEST_OBJ_DIR)/wasm_core.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lm
	./$@

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

# Interruptible-waiter primitive (replaces sleep-poll loops in bg threads).
$(TEST_OBJ_DIR)/test_waiter.o: $(TEST_DIR)/test_waiter.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
test_waiter: $(TEST_OBJ_DIR)/test_waiter.o $(TEST_OBJ_DIR)/waiter.o
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread

# Live MCP integration smoke test (not part of test_priorities — needs network +
# OPENROUTER_API_KEY). Loads dsco's full MCP config (incl. ./.mcp.json), proves the
# OpenRouter remote MCP server authenticates via the env-expanded Bearer header,
# discovers its tools, and fires one live models-list call. Links LIB_OBJS
# (mcp.o + its deps; excludes main/agent/orchestrator).
$(TEST_OBJ_DIR)/mcp_openrouter_smoke.o: $(TEST_DIR)/mcp_openrouter_smoke.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<
mcp_smoke: $(TEST_OBJ_DIR)/mcp_openrouter_smoke.o $(filter-out $(OBJ_DIR)/main.o, $(OBJS)) $(GSL_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ $^ $(LDFLAGS) $(RELEASE_LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

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
	test_learned_cost test_ooda_calibration test_session_memory test_memory_keep_score test_memory_classification test_wasm_core test_control_flow test_avian test_waiter test_math_corpus
	./test_recovery
	./test_stateful_atoms
	./test_plan_optimizer
	./test_plan_cache
	./test_learned_cost
	./test_ooda_calibration
	./test_session_memory
	./test_memory_keep_score
	./test_memory_classification
	./test_wasm_core
	./test_control_flow
	./test_avian
	./test_waiter
	./test_math_corpus $(TEST_DIR)/math_corpus.tsv

coverage: coverage_runner
	./coverage_runner

coverage_runner: $(TEST_COVERAGE_OBJS) $(GSL_COVERAGE_OBJS)
	$(CC) $(COVERAGE_CFLAGS) -o $@ $^ $(LDFLAGS) $(COVERAGE_LDFLAGS) $(LDLIBS)

asan: $(TARGET)-asan

# Embedded-data objects are plain byte arrays; the uninstrumented release
# objects are reused rather than rebuilding them per sanitizer.
$(TARGET)-asan: $(ASAN_OBJS) $(GSL_ASAN_OBJS) $(GENERATED_OBJS)
	$(CC) $(ASAN_CFLAGS) -o $@ $^ $(LDFLAGS) $(ASAN_LDFLAGS) $(LDLIBS)

ubsan: $(TARGET)-ubsan

$(TARGET)-ubsan: $(UBSAN_OBJS) $(GSL_UBSAN_OBJS) $(GENERATED_OBJS)
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
	@# Analyze with the SAME include/feature flags the real build uses, so
	@# clang-tidy resolves vendored GSL, hiredis and readline headers and sees
	@# the HAVE_* guarded declarations. Otherwise it reports spurious
	@# clang-diagnostic errors (undeclared time/dialog_*, missing gsl headers)
	@# and exits non-zero regardless of the (advisory) style warnings.
	clang-tidy $(SRCS) -- $(filter-out -MMD -MP,$(BASE_CFLAGS))

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

.PHONY: ip-check
ip-check:
	python3 scripts/ip_audit.py --check

docs: ip-check
	./scripts/gen_api_reference.sh
	./scripts/gen_tool_catalog.sh
	python3 scripts/index_constants_env.py --root .
	python3 scripts/gen_external_tool_catalog.py --root .
	python3 scripts/gen_repo_coverage.py --root .

claude-release-check:
	python3 scripts/sync_claude_release_family.py

docs-check: claude-release-check ip-check
	./scripts/gen_api_reference.sh --check
	./scripts/gen_tool_catalog.sh --check
	python3 scripts/index_constants_env.py --root . --check
	python3 scripts/gen_external_tool_catalog.py --root . --check
	python3 scripts/gen_repo_coverage.py --root . --check

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

release-hardened:
	python3 scripts/release_hardened.py $${DSCO_RELEASE_BINARY:-$(COSMO_TARGET).aarch64.elf}

release-hardened-native: $(TARGET)
	python3 scripts/release_hardened.py ./$(TARGET)

lint: format-check docs-check check-version

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(LITE_TARGET) $(DEBUG_TARGET) $(PROFILE_TARGET) dsc test_runner coverage_runner $(TARGET)-asan $(TARGET)-ubsan asan-test_runner ubsan-test_runner test_runner_asan test_runner_ubsan test_runner_asan_ubsan test_runner_tsan

install: $(TARGET) dsco-new $(LITE_TARGET) dsc
	install -d "$(BINDIR)"
	install -d "$(DSCO_SHARE_DIR)"
	sh scripts/install_atomic.sh "$(TARGET)" "$(BINDIR)/$(notdir $(TARGET))"
	install -m 755 $(LITE_TARGET) "$(BINDIR)/"
	install -m 755 dsc "$(BINDIR)/"
	sh scripts/install_atomic.sh dsco-new "$(BINDIR)/dsco-new"
	install -m 644 $(INC_DIR)/tool_embeddings.bin "$(DSCO_SHARE_DIR)/"
	install -d "$(DSCO_DIR)/sessions" "$(DSCO_DIR)/plugins" "$(DSCO_DIR)/debug"
	@canonical="$$(cd "$(BINDIR)" && pwd -P)/$(TARGET)"; \
	sync_path="$(DSCO_INSTALL_SYNC_PATH)"; \
	if [ "$$sync_path" = auto ]; then \
		default_dir="$$(cd "$(HOME)/.local/bin" 2>/dev/null && pwd -P)"; \
		if [ "$$canonical" = "$$default_dir/$(TARGET)" ]; then sync_path=1; else sync_path=0; fi; \
	fi; \
	if [ "$$sync_path" != 1 ]; then \
		echo "skipped PATH synchronization for non-default install prefix"; \
		exit 0; \
	fi; \
	printf '%s\n' "$$PATH" | tr ':' '\n' | while IFS= read -r dir; do \
		[ -n "$$dir" ] || continue; \
		candidate="$$dir/$(TARGET)"; \
		[ -e "$$candidate" ] || [ -L "$$candidate" ] || continue; \
		physical_dir="$$(cd "$$dir" 2>/dev/null && pwd -P)" || continue; \
		[ "$$physical_dir/$(TARGET)" != "$$canonical" ] || continue; \
		if [ -d "$$candidate" ] && [ ! -L "$$candidate" ]; then \
			echo "warning: cannot synchronize directory $$candidate" >&2; \
		elif [ -w "$$dir" ]; then \
			ln -sfn "$$canonical" "$$candidate"; \
			echo "linked $$candidate -> $$canonical"; \
		elif ! cmp -s "$$canonical" "$$candidate"; then \
			echo "warning: stale $$candidate is not writable; remove it or reinstall there with appropriate privileges" >&2; \
		fi; \
	done
	@echo "installed dsco, dsco-lite, dsc, dsco-new to $(BINDIR)/"
	@echo "installed tool_embeddings.bin to $(DSCO_SHARE_DIR)/"
	@echo "created $(DSCO_DIR)/{sessions,plugins,debug}"
	@echo "canonical dsco: $(BINDIR)/$(TARGET)"

uninstall:
	@canonical="$$(cd "$(BINDIR)" 2>/dev/null && pwd -P)/$(TARGET)"; \
	sync_path="$(DSCO_INSTALL_SYNC_PATH)"; \
	if [ "$$sync_path" = auto ]; then \
		default_dir="$$(cd "$(HOME)/.local/bin" 2>/dev/null && pwd -P)"; \
		if [ "$$canonical" = "$$default_dir/$(TARGET)" ]; then sync_path=1; else sync_path=0; fi; \
	fi; \
	[ "$$sync_path" = 1 ] || exit 0; \
	printf '%s\n' "$$PATH" | tr ':' '\n' | while IFS= read -r dir; do \
		[ -n "$$dir" ] || continue; \
		candidate="$$dir/$(TARGET)"; \
		[ -L "$$candidate" ] || continue; \
		[ "$$(readlink "$$candidate")" = "$$canonical" ] || continue; \
		rm -f "$$candidate"; \
	done
	rm -f "$(BINDIR)/$(TARGET)"
	rm -f "$(BINDIR)/$(LITE_TARGET)"
	rm -f "$(BINDIR)/dsc"
	rm -f "$(BINDIR)/dsco-new"
	rm -f "$(DSCO_SHARE_DIR)/tool_embeddings.bin"
	-rmdir "$(DSCO_SHARE_DIR)" 2>/dev/null || true
	@echo "removed $(BINDIR)/$(TARGET) and installer-managed PATH links"

ui-deps:
	pip install -r web/requirements.txt

ui: $(TARGET) ui-deps
	./$(TARGET) --ui

.PHONY: all debug dev clean install uninstall test coverage docs docs-check claude-release-check \
	profile profile-instrumented \
	asan ubsan asan-test ubsan-test test_runner_tsan test_plan_cache_tsan test_runner_asan format format-check \
	test-cli-flags \
	model-resolution-sim \
	fast fast-build fast-test fast-quick fast-syntax fast-changed fast-bench fast-doctor \
	changed-tests compile-commands build-report build-cache-doctor fast-objects time-trace ninja-file ninja-build \
	lint clang-tidy cppcheck static-analysis check-version \
	ui ui-deps bench-startup bench-tool bench-agent-loop bench-local \
	bench-sota bench-ttft bench-worker bench-size release-hardened release-hardened-native

# Tripwire quartet (SOTA_FRAMES_2026-09-02.md, T12; #1/#2 of 4 already ship
# as 'make test-gate-claims' and 'make docs-check' above). Deliberately
# cheap, read-only, no gate/RSI blast radius: observability signals only.
.PHONY: staleness-check revenue-check tripwires
staleness-check:
	./scripts/staleness_alarm.sh --check

revenue-check:
	./scripts/revenue_pace_reconciliation.sh --check

tripwires: staleness-check revenue-check
	@echo "tripwires: staleness + revenue-pace both ran (see exit code / output above)"

.PHONY: test-execution-kernel test-execution-spine-structure test-execution-spine-mcp test-tool-hooks test-self-swarm-core
test-execution-kernel:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude -o $(BUILD_DIR)/test_execution_kernel tests/test_execution_kernel.c src/execution_kernel.c src/tool_hooks.c src/process_capture.c src/env_config.c src/json_util.c src/crypto.c -lpthread
	$(BUILD_DIR)/test_execution_kernel

test-tool-hooks: $(TARGET)
	python3 tests/test_tool_hooks.py --binary "$(abspath $(TARGET))"

test-execution-spine-structure:
	python3 tests/test_execution_spine_structure.py

test-execution-spine-mcp: $(TARGET) test-execution-spine-structure
	python3 tests/test_execution_spine_mcp.py --binary "$(abspath $(TARGET))"

.PHONY: test-journal-concurrency test-execution-recovery test-context-normalization
test-journal-concurrency:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Iinclude -ffunction-sections -fdata-sections -o $(BUILD_DIR)/test_journal_concurrency tests/test_journal_concurrency.c src/json_util.c src/event_stream.c vendor/yyjson.c $(RELEASE_LDFLAGS) -lsqlite3 -lpthread -lm
	$(BUILD_DIR)/test_journal_concurrency

test-execution-recovery: $(TARGET)
	python3 tests/test_execution_recovery.py --binary "$(abspath $(TARGET))"

test-context-normalization:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Iinclude -ffunction-sections -fdata-sections -o $(BUILD_DIR)/test_context_normalization tests/test_context_normalization.c src/llm.c src/tool_integrity_index.c src/json_util.c src/json_fast.c $(RELEASE_LDFLAGS)
	$(BUILD_DIR)/test_context_normalization

.PHONY: test-process-capture-lifecycle test-agent-batch-order test-harness-reliability
test-process-capture-lifecycle:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Iinclude -o $(BUILD_DIR)/test_process_capture_lifecycle tests/test_process_capture_lifecycle.c src/process_capture.c src/json_util.c
	$(BUILD_DIR)/test_process_capture_lifecycle

test-agent-batch-order: $(TARGET)
	python3 tests/test_agent_batch_order_binary.py --binary "$(abspath $(TARGET))"

test-harness-reliability: test-journal-concurrency test-context-normalization test_dynamic_tool_cache test-process-capture-lifecycle test_pty_session test-execution-recovery test-agent-batch-order test-execution-spine-mcp test-tool-hooks test-gate-claims

test: test-harness-reliability

.PHONY: test-input-budget test_composer_input test-interactive-core
test-input-budget:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_input_budget tests/test_input_budget.c tests/input_budget_archive_stubs.c src/input_budget.c vendor/yyjson.c
	$(BUILD_DIR)/test_input_budget

$(BUILD_DIR)/composer_input_fixture: tests/composer_input_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ tests/composer_input_fixture.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

test_composer_input: $(BUILD_DIR)/composer_input_fixture
	python3 tests/test_composer_input.py --fixture $(BUILD_DIR)/composer_input_fixture
	python3 tests/test_tui_composer_zoom.py --fixture $(BUILD_DIR)/composer_input_fixture

	@if command -v kitty >/dev/null 2>&1; then \
	  kitty +runpy 'exec(compile(open("tests/test_tui_kitty_reflow.py").read(),"test","exec"), globals())'; \
	else echo 'Kitty headless reflow: SKIP (kitty not installed)'; fi

.PHONY: test-ask-user-question-pty
test-ask-user-question-pty: $(TARGET)
	python3 tests/test_ask_user_question_pty.py --binary ./$(TARGET) --output reports/ask-question-pty

$(BUILD_DIR)/voice_session_fixture: tests/voice_session_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ $< $(filter-out $(OBJ_DIR)/realtime.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)

.PHONY: test-voice-session
test-voice-session: $(BUILD_DIR)/voice_session_fixture $(TARGET)
	python3 tests/test_voice_session.py --fixture $(BUILD_DIR)/voice_session_fixture
	python3 tests/test_voice_session_binary.py --binary ./$(TARGET)

# All real agent/orchestrator code; only microphone and speaker are replaced
# at link time. Production dsco has no transcript-injection test switches.
$(BUILD_DIR)/dsco_native_voice_fixture: tests/native_voice_agent_backend.c $(OBJS) $(GSL_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ $< $(filter-out $(OBJ_DIR)/native_voice_macos.o,$(OBJS)) $(GSL_OBJS) $(LDFLAGS) $(LDLIBS)

.PHONY: test-native-voice-agent
test-native-voice-agent: $(BUILD_DIR)/dsco_native_voice_fixture
	python3 tests/test_native_voice_agent.py --binary $(BUILD_DIR)/dsco_native_voice_fixture

$(BUILD_DIR)/test_native_voice: tests/test_native_voice.c src/native_voice.c include/native_voice.h include/native_voice_backend.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_native_voice.c src/native_voice.c -lpthread

$(BUILD_DIR)/native_voice_fixture: tests/native_voice_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ $< $(filter-out $(OBJ_DIR)/native_voice_macos.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)

.PHONY: test-native-voice
test-native-voice: $(BUILD_DIR)/test_native_voice $(BUILD_DIR)/native_voice_fixture
	$(BUILD_DIR)/test_native_voice
	python3 tests/test_native_voice.py --fixture $(BUILD_DIR)/native_voice_fixture

$(BUILD_DIR)/stream_completion_fixture: tests/stream_completion_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ tests/stream_completion_fixture.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/gateway_provider_fixture: tests/gateway_provider_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ tests/gateway_provider_fixture.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/kimi_pool_fixture: tests/kimi_pool_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ tests/kimi_pool_fixture.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

.PHONY: test-kimi-pool
test-kimi-pool: $(BUILD_DIR)/kimi_pool_fixture $(TARGET)
	python3 tests/test_kimi_pool.py --fixture $(BUILD_DIR)/kimi_pool_fixture
	python3 tests/test_kimi_pool.py --binary ./$(TARGET)

.PHONY: test-gateway-providers
test-gateway-providers: $(BUILD_DIR)/gateway_provider_fixture
	python3 tests/test_gateway_providers.py --fixture $(BUILD_DIR)/gateway_provider_fixture

.PHONY: test-stream-completion test-interactive-cost test-input-budget-binary
test-stream-completion: $(BUILD_DIR)/stream_completion_fixture
	python3 tests/test_stream_completion.py --fixture $(BUILD_DIR)/stream_completion_fixture --output $(BUILD_DIR)/stream-completion-results
	python3 tests/test_native_provider_transport.py --fixture $(BUILD_DIR)/stream_completion_fixture

test-interactive-cost: $(TARGET)
	python3 tests/test_interactive_cost_binary.py --binary $(abspath $(TARGET))

test-input-budget-binary: $(TARGET)
	python3 tests/test_input_budget_binary.py --binary $(abspath $(TARGET))

.PHONY: test-interactive-stream-recovery test-critical-stamp
test-interactive-stream-recovery: $(TARGET)
	python3 tests/test_interactive_stream_recovery.py --binary $(abspath $(TARGET)) --output $(BUILD_DIR)/interactive-stream-results

test-critical-stamp: $(TARGET) test_native_transcript_review
	python3 tests/test_critical_stamp.py --binary $(abspath $(TARGET))

test-interactive-core: test-input-budget test_composer_input test-stream-completion test-interactive-cost test-input-budget-binary test-interactive-stream-recovery test-critical-stamp test-ask-user-question-pty

test: test-interactive-core

.PHONY: test-goal-queue
.PHONY: test-goal-native
test-goal-native:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_goal_native tests/test_goal_native.c src/goal_native.c src/native_session_details.c -lm
	$(BUILD_DIR)/test_goal_native

test-goal-queue:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_goal_queue tests/test_goal_queue.c src/goal_queue.c src/work_unit.c src/crypto.c src/json_util.c vendor/yyjson.c $(ASAN_LDFLAGS)
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_goal_queue

.PHONY: test-work-algebra
test-work-algebra:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_work_algebra tests/test_work_algebra.c src/work_algebra.c src/config_surface.c $(ASAN_LDFLAGS) -lm
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_work_algebra

.PHONY: test-config-surface
test-config-surface:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_config_surface tests/test_config_surface.c src/config_surface.c $(ASAN_LDFLAGS) -lm
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_config_surface

.PHONY: test-request-intent
test-request-intent:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_request_intent tests/test_request_intent.c src/request_intent.c $(ASAN_LDFLAGS)
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_request_intent

.PHONY: test-dsco-meta
test-dsco-meta:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_dsco_meta tests/test_dsco_meta.c src/dsco_c.c $(ASAN_LDFLAGS)
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_dsco_meta
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude -o $(BUILD_DIR)/test_dsco_meta_c11 tests/test_dsco_meta.c src/dsco_c.c
	$(BUILD_DIR)/test_dsco_meta_c11

.PHONY: test-goal-controller-binary
test-goal-controller-binary: $(TARGET)
	python3 tests/test_goal_controller_binary.py --binary "$(abspath $(TARGET))"

test-self-swarm-core:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude -o $(BUILD_DIR)/test_execution_verification tests/test_execution_layer_verification.c src/execution_layer.c src/json_util.c -lm
	$(BUILD_DIR)/test_execution_verification
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude -o $(BUILD_DIR)/test_headless_accounting tests/test_headless_accounting.c src/headless_accounting.c src/inference_cost.c src/json_util.c -lm
	$(BUILD_DIR)/test_headless_accounting
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude -o $(BUILD_DIR)/test_value_ledger_failures tests/test_value_ledger_failures.c src/value_ledger.c src/json_util.c -lm
	$(BUILD_DIR)/test_value_ledger_failures

.PHONY: test-deepseek-pricing
test-deepseek-pricing:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude $$(pkg-config --cflags libcurl) -o $(BUILD_DIR)/test_deepseek_pricing tests/test_deepseek_pricing.c src/deepseek_pricing.c $$(pkg-config --libs libcurl) -lpthread
	$(BUILD_DIR)/test_deepseek_pricing

.PHONY: test-parallel-pricing
test-parallel-pricing:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -Iinclude -o $(BUILD_DIR)/test_parallel_pricing tests/test_parallel_pricing.c src/parallel_pricing.c -lm
	$(BUILD_DIR)/test_parallel_pricing

.PHONY: test-cost-frontier
test-cost-frontier:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -Iinclude -o $(BUILD_DIR)/test_cost_frontier tests/test_cost_frontier.c src/cost_frontier.c src/json_util.c -lm
	$(BUILD_DIR)/test_cost_frontier

.PHONY: test-seat-take
test-seat-take:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -Iinclude -o $(BUILD_DIR)/test_seat_take tests/test_seat_take.c src/seat_take.c -lm
	$(BUILD_DIR)/test_seat_take

.PHONY: test-jev-decide
test-jev-decide:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_jev_decide tests/test_jev_decide.c src/jev_decide.c src/crypto.c src/json_util.c vendor/yyjson.c -lm -lpthread
	$(BUILD_DIR)/test_jev_decide

.PHONY: test-continuity-regression
test-continuity-regression: $(TARGET) test_context_eviction test-compact-policy
	python3 tests/test_rolling_headless.py ./$(TARGET)
	python3 tests/test_provider_continuity_binary.py ./$(TARGET)
	python3 tests/test_provider_continuity_binary.py ./$(TARGET) --interactive
	python3 tests/test_provider_continuity_binary.py ./$(TARGET) --disabled

.PHONY: test-compact-policy
test-compact-policy:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_compact_policy tests/test_compact_policy.c src/compact_policy.c -lm
	$(BUILD_DIR)/test_compact_policy

.PHONY: test-jev-gates
test-jev-gates:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_jev_gates tests/test_jev_gates.c src/jev_gates.c src/jev_decide.c src/crypto.c src/json_util.c vendor/yyjson.c -lm -lpthread
	$(BUILD_DIR)/test_jev_gates

.PHONY: test-repl-sem
test-repl-sem:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_repl_sem tests/test_repl_sem.c src/repl_sem.c src/ai_repl.c -lm
	$(BUILD_DIR)/test_repl_sem

.PHONY: test_buffer_cli test_buffer_views
test_buffer_cli:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_buffer_cli.c src/buffer_cli.c src/json_fast.c
	$(BUILD_DIR)/$@

test_buffer_views: $(TARGET)
	python3 tests/test_buffer_views_mcp.py --binary ./$(TARGET)

.PHONY: test_buffer_store test_buffer_view test_buffer_ui test_pixel_scene_lifetime
test_buffer_store:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -D_DARWIN_C_SOURCE -D_GNU_SOURCE -o $(BUILD_DIR)/$@ tests/test_buffer_store.c src/buffer_store.c src/json_util.c src/crypto.c src/json_fast.c -lsqlite3 -lpthread
	$(BUILD_DIR)/$@

test_buffer_view:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_buffer_view.c src/buffer_view.c src/buffer_textedit.c src/json_fast.c
	$(BUILD_DIR)/$@

test_buffer_ui:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_buffer_ui.c src/buffer_ui.c src/json_fast.c
	$(BUILD_DIR)/$@

test_pixel_scene_lifetime: tests/test_pixel_scene_lifetime.c src/pixel_tui.c include/pixel_tui.h $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ tests/test_pixel_scene_lifetime.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

.PHONY: test_buffer_slash test_native_scene_visual
test_buffer_slash: $(TARGET)
	python3 tests/test_buffer_slash.py --binary ./$(TARGET)
	python3 tests/test_buffer_slash.py --binary ./$(TARGET) --native

test_native_scene_visual: $(TARGET) test_pixel_scene_lifetime
	python3 tests/test_native_scene_visual.py --binary ./$(TARGET)

.PHONY: test_agent_tool_routing test_agent_tool_routing_binary
test: test_agent_tool_routing test_agent_tool_routing_binary

test_agent_tool_routing:
	python3 tests/test_agent_tool_routing.py

test_agent_tool_routing_binary: $(TARGET)
	python3 tests/test_agent_tool_routing_binary.py --binary ./$(TARGET)

.PHONY: test_tool_grounding
test_tool_grounding:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_tool_grounding.c src/tool_grounding.c src/json_util.c src/json_fast.c
	$(BUILD_DIR)/$@

.PHONY: test_tool_grounding_requests test_selected_mcp_gate
test_tool_grounding_requests: tests/test_tool_grounding_requests.c src/provider.c include/tool_grounding.h include/llm.h $(filter-out $(OBJ_DIR)/provider.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ tests/test_tool_grounding_requests.c $(filter-out $(OBJ_DIR)/provider.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_selected_mcp_gate: $(TARGET)
	python3 tests/test_selected_mcp_gate.py --binary ./$(TARGET)

.PHONY: test_mcp_catalog_retry
test_mcp_catalog_retry:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -D_DARWIN_C_SOURCE -D_GNU_SOURCE -ffunction-sections -fdata-sections -o $(BUILD_DIR)/$@ tests/test_mcp_catalog_retry.c src/mcp_response.c src/json_util.c src/json_fast.c -Wl,-dead_strip -lcurl -lm
	$(BUILD_DIR)/$@

.PHONY: test_swarm_progress
.PHONY: test_timeout_policy test_timeout_watchdog
.PHONY: test-shell-job-events
test-shell-job-events:
	@mkdir -p $(BUILD_DIR)/job-events
	$(CC) $(TEST_CFLAGS) -pthread -o $(BUILD_DIR)/job-events/events tests/test_shell_job_events.c src/shell_job_events.c
	$(BUILD_DIR)/job-events/events
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/job-events/publication tests/test_shell_job_publication.c src/shell_job_events.c src/timeout_policy.c src/env_config.c src/json_util.c vendor/yyjson.c -lm
	$(BUILD_DIR)/job-events/publication
test_timeout_policy: tests/test_timeout_policy.c src/timeout_policy.c include/timeout_policy.h include/tools.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_timeout_policy.c src/timeout_policy.c src/env_config.c src/json_util.c vendor/yyjson.c -lm
	$(BUILD_DIR)/$@

timeout_watchdog_fixture: tests/test_timeout_watchdog.c $(TUI_TEST_LIB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_timeout_watchdog.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

test_timeout_watchdog: timeout_watchdog_fixture
	$(BUILD_DIR)/timeout_watchdog_fixture

.PHONY: test_timeout_binary
test_timeout_binary: $(TARGET)
	python3 tests/test_timeout_binary.py --binary ./$(TARGET) --artifact-dir $(BUILD_DIR)/timeout-binary

swarm_lifecycle_fixture: tests/test_swarm_lifecycle.c $(TUI_TEST_LIB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_swarm_lifecycle.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

.PHONY: test_swarm_lifecycle
swarm_cancel_failure_fixture: tests/test_swarm_cancel_failure.c src/swarm_lifecycle.c include/swarm_lifecycle.h include/swarm.h src/json_util.c include/json_util.h vendor/yyjson.c vendor/yyjson.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -ffunction-sections -fdata-sections -o $(BUILD_DIR)/$@ tests/test_swarm_cancel_failure.c src/swarm_lifecycle.c src/json_util.c vendor/yyjson.c $(if $(filter Darwin,$(UNAME_S)),-Xlinker -dead_strip,-Xlinker --gc-sections) -lm

test_swarm_lifecycle: swarm_lifecycle_fixture swarm_cancel_failure_fixture
	$(BUILD_DIR)/swarm_lifecycle_fixture
	$(BUILD_DIR)/swarm_cancel_failure_fixture

swarm_progress_fixture: tests/test_swarm_progress.c $(TUI_TEST_LIB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_swarm_progress.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

test_swarm_progress: swarm_progress_fixture
	$(BUILD_DIR)/swarm_progress_fixture
	$(BUILD_DIR)/swarm_progress_fixture tool
	$(BUILD_DIR)/swarm_progress_fixture negative-tool
	$(BUILD_DIR)/swarm_progress_fixture condition
	$(BUILD_DIR)/swarm_progress_fixture kill-wait
	$(BUILD_DIR)/swarm_progress_fixture killed-reap
	$(BUILD_DIR)/swarm_progress_fixture concurrent 64
	$(BUILD_DIR)/swarm_progress_fixture busy 16
	$(BUILD_DIR)/swarm_progress_fixture anthropic
	$(BUILD_DIR)/swarm_progress_fixture openai
	$(BUILD_DIR)/swarm_progress_fixture bench 16
	$(BUILD_DIR)/swarm_progress_fixture bench 64
	$(BUILD_DIR)/swarm_progress_fixture bench 100

.PHONY: test_invoke_tool_tier
test_invoke_tool_tier: $(TARGET)
	python3 tests/test_invoke_tool_tier.py --binary ./$(TARGET)

.PHONY: test_swarm_scale
test_swarm_scale: tests/test_swarm_scale.c src/swarm_scale.c include/swarm_scale.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -O2 -o $(BUILD_DIR)/$@ tests/test_swarm_scale.c src/swarm_scale.c src/json_util.c src/crypto.c vendor/yyjson.c -lm -lpthread
	$(BUILD_DIR)/$@

.PHONY: test_swarm_scale_mcp
test_swarm_scale_mcp: $(TARGET)
	python3 tests/test_swarm_scale_mcp.py --owned-interplay --artifact-dir $(BUILD_DIR)/swarm-scale-mcp

.PHONY: test_swarm_hierarchy test_swarm_hierarchy_mcp
test_swarm_hierarchy: tests/test_swarm_hierarchy.c src/swarm_hierarchy.c include/swarm_hierarchy.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -O2 -o $(BUILD_DIR)/$@ tests/test_swarm_hierarchy.c src/swarm_hierarchy.c src/agent_interop.c src/agent_run_outcome.c src/subscription_paths.c src/process_capture.c src/json_util.c vendor/yyjson.c -lm -lpthread
	$(BUILD_DIR)/$@

test_swarm_hierarchy_mcp: $(TARGET)
	python3 tests/test_swarm_hierarchy_mcp.py --binary ./$(TARGET) --artifact-dir $(BUILD_DIR)/swarm-hierarchy-mcp

.PHONY: test_native_windows
.PHONY: test_native_tasks
test_native_tasks:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ tests/test_native_tasks.c src/native_windows.c src/native_app.c src/native_window_layout.c src/native_factory.c src/native_task_service.c src/native_buffer_editor.c src/crypto.c src/json_util.c src/json_fast.c -lm -lpthread
	$(BUILD_DIR)/$@

.PHONY: test_native_apps
test_native_apps:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_apps.c src/native_app.c src/native_app_store.c src/native_workbench_state.c src/native_windows.c src/native_window_layout.c src/native_factory.c src/native_buffer_editor.c src/crypto.c src/json_util.c src/json_fast.c -lm -lpthread
	$(BUILD_DIR)/$@

test_native_windows:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_windows.c src/native_windows.c src/native_app.c src/native_window_layout.c src/native_factory.c src/crypto.c src/native_buffer_editor.c src/json_util.c src/json_fast.c -lpthread
	$(BUILD_DIR)/$@

.PHONY: test_native_tool_cadence
test_native_tool_cadence: $(TARGET)
	python3 tests/test_native_tool_cadence.py --binary ./$(TARGET) --output $(BUILD_DIR)/native-tool-cadence.json

.PHONY: test_native_buffer_edit
test_native_buffer_edit: $(TARGET)
	python3 tests/test_native_buffer_edit.py --binary ./$(TARGET)

.PHONY: test_native_trace
test_native_trace:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_trace.c src/native_trace.c src/json_fast.c -lpthread -lm
	./$(BUILD_DIR)/$@

.PHONY: test_native_activity
.PHONY: test_native_zoom
.PHONY: test_native_transcript_review test_native_startup_card test_native_table_blocks

test_native_table_blocks: tests/test_native_table_blocks.c src/native_table_blocks.c src/presentation_blocks.c include/native_table_blocks.h
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_table_blocks.c src/native_table_blocks.c src/presentation_blocks.c
	$(BUILD_DIR)/$@

test_native_startup_card: tests/test_native_startup_card.c src/native_startup_card.c include/native_startup_card.h
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_startup_card.c src/native_startup_card.c
	$(BUILD_DIR)/$@
.PHONY: test_native_render_perf
.PHONY: test_native_motion test_self_improve_progress
.PHONY: test_self_improve_mcp

test_self_improve_mcp: $(TARGET)
	python3 tests/test_self_improve_mcp.py --binary ./$(TARGET) --output $(BUILD_DIR)/self-improve-mcp.json

test_native_motion: tests/test_native_motion.c tests/test_native_transcript_review.c src/pixel_tui.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(CFLAGS) -DDSCO_INTERNAL_TESTS -fcommon -o $(BUILD_DIR)/$@ tests/test_native_motion.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_self_improve_progress: tests/test_self_improve_progress.c src/self_improve.c src/json_util.c src/json_fast.c src/rsi_curriculum.c include/self_improve.h
	$(CC) $(filter-out -D_FORTIFY_SOURCE=2,$(TEST_CFLAGS)) -O1 -fsanitize=address,undefined -o $(BUILD_DIR)/$@ tests/test_self_improve_progress.c src/self_improve.c src/json_util.c src/json_fast.c src/rsi_curriculum.c -lm
	$(BUILD_DIR)/$@

test_native_render_perf: tests/test_native_render_perf.c tests/test_native_transcript_review.c src/pixel_tui.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(CFLAGS) -DDSCO_INTERNAL_TESTS -fcommon -o $(BUILD_DIR)/$@ tests/test_native_render_perf.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

.PHONY: test_native_composer_perf test_native_readability
test_native_readability: tests/test_native_readability.c tests/test_native_transcript_review.c src/pixel_tui.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(CFLAGS) -DDSCO_INTERNAL_TESTS -fcommon -o $(BUILD_DIR)/$@ tests/test_native_readability.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@
	python3 tests/test_native_swarm_echo.py

test_native_composer_perf: tests/test_native_composer_perf.c tests/test_native_transcript_review.c src/pixel_tui.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(CFLAGS) -DDSCO_INTERNAL_TESTS -fcommon -o $(BUILD_DIR)/$@ tests/test_native_composer_perf.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_native_transcript_review: tests/test_native_transcript_review.c src/pixel_tui.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(CFLAGS) -DDSCO_INTERNAL_TESTS -fcommon -o $(BUILD_DIR)/$@ tests/test_native_transcript_review.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_native_zoom: tests/test_native_zoom.c src/pixel_tui.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(CFLAGS) -DDSCO_INTERNAL_TESTS -fcommon -o $(BUILD_DIR)/$@ tests/test_native_zoom.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_native_activity: tests/test_native_activity.c src/pixel_tui.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(CFLAGS) -DDSCO_INTERNAL_TESTS -fcommon -o $(BUILD_DIR)/$@ tests/test_native_activity.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

.PHONY: test_native_trace_ui
test_native_trace_ui:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_trace_ui.c src/native_trace_ui.c src/native_trace.c src/json_fast.c -lpthread -lm
	$(BUILD_DIR)/$@

.PHONY: test_native_trace_controls
test_native_trace_controls: $(TARGET)
	python3 tests/test_native_trace_controls.py --binary ./$(TARGET) --output $(BUILD_DIR)/native-trace-controls.json

.PHONY: test_native_session_details_pty
test_native_session_details_pty: $(TARGET)
	python3 tests/test_native_session_details_pty.py --binary ./$(TARGET) --output $(BUILD_DIR)/native-session-details-pty.json

.PHONY: test_native_session_details
test_native_session_details: tests/test_native_session_details.c tests/test_pixel_native_windows.c src/pixel_tui.c include/pixel_tui.h $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ tests/test_native_session_details.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

.PHONY: test_pixel_native_windows test_native_windows_visual
.PHONY: test_native_window_cache
test_native_window_cache: tests/test_native_window_cache.c src/pixel_tui.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ tests/test_native_window_cache.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_pixel_native_windows: tests/test_pixel_native_windows.c src/pixel_tui.c include/pixel_tui.h $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ tests/test_pixel_native_windows.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_native_windows_visual: $(TARGET) test_pixel_native_windows
	python3 tests/test_native_windows_visual.py --binary ./$(TARGET)

.PHONY: test_native_windows_composer
$(BUILD_DIR)/native_windows_composer_fixture: tests/native_windows_composer_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ tests/native_windows_composer_fixture.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

test_native_windows_composer: $(BUILD_DIR)/native_windows_composer_fixture
	python3 tests/test_native_windows_composer.py

.PHONY: test_native_window_tool
test_native_window_tool:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_window_tool.c src/native_window_tool.c src/native_task_service.c src/native_windows.c src/native_app.c src/native_window_layout.c src/native_factory.c src/native_buffer_editor.c src/crypto.c src/json_fast.c -lpthread
	$(BUILD_DIR)/$@

.PHONY: test_native_writing_editor
test_native_writing_editor:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_writing_editor.c src/native_window_tool.c src/native_task_service.c src/native_windows.c src/native_app.c src/native_window_layout.c src/native_factory.c src/native_buffer_editor.c src/crypto.c src/json_fast.c -lpthread
	$(BUILD_DIR)/$@

$(BUILD_DIR)/native_writing_composer_fixture: tests/native_writing_composer_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ tests/native_writing_composer_fixture.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

.PHONY: test_buffer_textedit
test_buffer_textedit:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_buffer_textedit.c src/buffer_textedit.c src/json_fast.c
	$(BUILD_DIR)/$@

.PHONY: test_mcp_response
test_mcp_response:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_mcp_response tests/test_mcp_response.c src/mcp_response.c src/json_fast.c
	$(BUILD_DIR)/test_mcp_response
	$(CC) $(TEST_CFLAGS) -D_DARWIN_C_SOURCE -D_GNU_SOURCE -ffunction-sections -fdata-sections -o $(BUILD_DIR)/mcp_http_response_fixture tests/mcp_http_response_fixture.c src/mcp_response.c src/http_pool.c src/json_util.c src/json_fast.c $(if $(filter Darwin,$(UNAME_S)),-Xlinker -dead_strip,-Xlinker --gc-sections) -lcurl -lm -lpthread
	python3 tests/test_mcp_http_response.py

.PHONY: test-native-transport-reuse
test-native-transport-reuse:
	python3 tests/test_native_transport_reuse.py --sanitize --output $(BUILD_DIR)/native-transport-reuse.json

# Offline regressions for scoped native workers and addressed IPC delivery.
.PHONY: test-worker-runtime
test-worker-runtime:
	python3 tests/test_parallel_coordination.py
	python3 tests/test_worker_runtime_delivery.py
	python3 tests/test_swarm_instance_forwarding.py
	python3 tests/test_swarm_create_budget.py
	python3 tests/test_agent_ipc_boundary.py
	python3 tests/test_ipc_task_fencing.py

.PHONY: test_presentation_blocks test_transcript_composer

.PHONY: test_code_edit test_code_edit_mcp
test_code_edit:
	$(CC) $(TEST_CFLAGS) -fsanitize=address,undefined -g -o $(BUILD_DIR)/$@ tests/test_code_edit.c src/code_edit.c src/skill_lifecycle.c src/edit_diff.c src/crypto.c src/json_util.c vendor/yyjson.c -lm -lpthread
	$(BUILD_DIR)/$@

test_code_edit_mcp: $(TARGET)
	python3 tests/test_code_edit_mcp.py --binary ./$(TARGET) --artifact-dir $(BUILD_DIR)/code-edit-mcp

.PHONY: test_skill_selection_pty
test_skill_selection_pty: $(TARGET)
	python3 tests/test_skill_selection_pty.py --binary ./$(TARGET) --output $(BUILD_DIR)/skill-selection-pty.json

.PHONY: test_harness_receipt_scene
test_harness_receipt_scene: tests/harness_receipt_scene.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/harness_receipt_scene tests/harness_receipt_scene.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/harness_receipt_scene $(BUILD_DIR)/harness-scenes

test_presentation_blocks:
	$(CC) $(TEST_CFLAGS) -DPRESENTATION_STUB_PAGER -fsanitize=address,undefined -g -Iinclude -o $(BUILD_DIR)/test_presentation_blocks tests/test_presentation_blocks.c src/presentation_blocks.c src/rich_text.c src/presentation_json.c src/transcript_view.c src/json_util.c src/json_fast.c -lm
	$(BUILD_DIR)/test_presentation_blocks

$(BUILD_DIR)/transcript_composer_fixture: tests/transcript_composer_fixture.c $(TUI_TEST_LIB_OBJS)
	$(CC) $(TEST_CFLAGS) -fcommon -o $@ tests/transcript_composer_fixture.c $(TUI_TEST_LIB_OBJS) $(LDFLAGS) $(LDLIBS)

test_transcript_composer: $(BUILD_DIR)/transcript_composer_fixture
	python3 tests/test_transcript_composer.py

# Native buffer refresh/reflow and capture-start regression contracts.
.PHONY: test_native_buffer_lifecycle test_native_trace_activation
test_native_buffer_lifecycle:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_buffer_lifecycle.c src/native_window_tool.c src/native_task_service.c src/native_windows.c src/native_app.c src/native_window_layout.c src/native_factory.c src/native_buffer_editor.c src/crypto.c src/json_fast.c -lpthread
	$(BUILD_DIR)/$@ all

.PHONY: test_native_transport_recovery
test_native_transport_recovery: tests/test_native_transport_recovery.c tests/test_pixel_native_windows.c src/pixel_tui.c include/pixel_tui.h $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ tests/test_native_transport_recovery.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

test_native_trace_activation: tests/test_native_trace_activation.c tests/test_pixel_native_windows.c src/pixel_tui.c include/pixel_tui.h $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS))
	$(CC) $(TEST_CFLAGS) -fcommon -o $(BUILD_DIR)/$@ tests/test_native_trace_activation.c $(filter-out $(OBJ_DIR)/pixel_tui.o,$(TUI_TEST_LIB_OBJS)) $(LDFLAGS) $(LDLIBS)
	$(BUILD_DIR)/$@

.PHONY: test-mcp-diagnostics
test-mcp-diagnostics:
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_mcp_diagnostics tests/test_mcp_diagnostics.c src/mcp_diagnostics.c src/json_util.c src/json_fast.c -lpthread -lm
	$(BUILD_DIR)/test_mcp_diagnostics

# Native subscription scheduling: offline fixtures only, no inference or UI.
.PHONY: test-subscription-scheduling
test-subscription-scheduling: $(TARGET)
	python3 tests/test_subscription_gate_wait.py --output $(BUILD_DIR)/subscription-scheduling/gate
	python3 tests/test_provider_subscription_wait.py --output $(BUILD_DIR)/subscription-scheduling/wrapper
	python3 tests/test_provider_admission.py --output $(BUILD_DIR)/subscription-scheduling/admission
	python3 tests/test_subscription_scheduling_binary.py --binary ./$(TARGET) --output $(BUILD_DIR)/subscription-scheduling/binary

test: test-subscription-scheduling

.PHONY: test-run-diagnostic
test-run-diagnostic:
	python3 tests/test_run_diagnostic.py

test: test-run-diagnostic

# Cooperative async/await scheduler + native reactor and portable fallback.
.PHONY: async-library async-demo
async-library: $(BUILD_DIR)/libdsco_async.a
$(BUILD_DIR)/async-library/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)/async-library
	$(CC) $(CFLAGS) -pthread -c -o $@ $<
$(BUILD_DIR)/libdsco_async.a: $(ASYNC_LIBRARY_OBJS)
	$(AR) rcs $@ $^
async-demo: async-library
	$(CC) $(CFLAGS) -pthread -o $(BUILD_DIR)/async-library/demo examples/async_graph.c $(BUILD_DIR)/libdsco_async.a
	$(BUILD_DIR)/async-library/demo
-include $(ASYNC_LIBRARY_OBJS:.o=.d)

.PHONY: test-async-structures test-async-structures-sanitize
test-async-structures:
	@mkdir -p $(BUILD_DIR)/async-structures
	$(CC) $(TEST_CFLAGS) -pthread -o $(BUILD_DIR)/async-structures/native $(ASYNC_STRUCTURE_SRCS)
	$(BUILD_DIR)/async-structures/native
	$(CC) $(TEST_CFLAGS) -pthread -DEV_FORCE_POLL -o $(BUILD_DIR)/async-structures/portable $(ASYNC_STRUCTURE_SRCS)
	$(BUILD_DIR)/async-structures/portable
	$(CC) $(TEST_CFLAGS) -pthread -DDSCO_POOL_FORCE_PTHREAD -o $(BUILD_DIR)/async-structures/pthread $(ASYNC_STRUCTURE_SRCS)
	$(BUILD_DIR)/async-structures/pthread
	$(CC) $(TEST_CFLAGS) -pthread -o $(BUILD_DIR)/async-structures/publication tests/test_async_publication.c src/event_loop.c src/event_wakeup.c
	$(BUILD_DIR)/async-structures/publication

test-async-structures-sanitize:
	@mkdir -p $(BUILD_DIR)/async-structures
	$(CC) $(ASAN_UBSAN_CFLAGS) -pthread -o $(BUILD_DIR)/async-structures/sanitize $(ASYNC_STRUCTURE_SRCS)
	$(BUILD_DIR)/async-structures/sanitize

.PHONY: test-async-structures-tsan
test-async-structures-tsan:
	@mkdir -p $(BUILD_DIR)/async-structures
	$(CC) $(TSAN_CFLAGS) -pthread -o $(BUILD_DIR)/async-structures/tsan $(ASYNC_STRUCTURE_SRCS)
	$(BUILD_DIR)/async-structures/tsan
	$(CC) $(TSAN_CFLAGS) -pthread -DDSCO_POOL_FORCE_PTHREAD -o $(BUILD_DIR)/async-structures/tsan-pthread $(ASYNC_STRUCTURE_SRCS)
	$(BUILD_DIR)/async-structures/tsan-pthread

.PHONY: test-scheduler-async
test-scheduler-async:
	@mkdir -p $(BUILD_DIR)/async-await
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/async-await/test_event_loop_backend tests/test_event_loop_backend.c src/event_loop.c src/event_wakeup.c
	$(BUILD_DIR)/async-await/test_event_loop_backend
	$(CC) $(TEST_CFLAGS) -DEV_FORCE_POLL -o $(BUILD_DIR)/async-await/test_event_loop_backend_poll tests/test_event_loop_backend.c src/event_loop.c src/event_wakeup.c
	$(BUILD_DIR)/async-await/test_event_loop_backend_poll
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/async-await/test_scheduler_async tests/test_scheduler_async.c src/scheduler.c src/scheduler_lifetime.c src/scheduler_io.c src/event_loop.c src/event_wakeup.c
	$(BUILD_DIR)/async-await/test_scheduler_async
	$(CC) $(TEST_CFLAGS) -DEV_FORCE_POLL -o $(BUILD_DIR)/async-await/test_scheduler_async_poll tests/test_scheduler_async.c src/scheduler.c src/scheduler_lifetime.c src/scheduler_io.c src/event_loop.c src/event_wakeup.c
	$(BUILD_DIR)/async-await/test_scheduler_async_poll
	$(CC) $(TEST_CFLAGS) -pthread -o $(BUILD_DIR)/async-await/test_event_driven_wait tests/test_event_driven_wait.c src/scheduler.c src/scheduler_lifetime.c src/scheduler_io.c src/event_loop.c src/event_wakeup.c
	$(BUILD_DIR)/async-await/test_event_driven_wait
	$(CC) $(TEST_CFLAGS) -pthread -DEV_FORCE_POLL -o $(BUILD_DIR)/async-await/test_event_driven_wait_poll tests/test_event_driven_wait.c src/scheduler.c src/scheduler_lifetime.c src/scheduler_io.c src/event_loop.c src/event_wakeup.c
	$(BUILD_DIR)/async-await/test_event_driven_wait_poll

.PHONY: test-runtime-ownership
OWNERSHIP_SCHED_SRCS = tests/test_scheduler_lifetime.c src/ooda.c src/error.c src/scheduler.c src/scheduler_lifetime.c src/scheduler_io.c src/event_loop.c src/event_wakeup.c
test-runtime-ownership:
	@mkdir -p $(BUILD_DIR)/runtime-ownership
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/runtime-ownership/scheduler $(OWNERSHIP_SCHED_SRCS) -lm
	$(BUILD_DIR)/runtime-ownership/scheduler
	$(CC) $(TEST_CFLAGS) -DEV_FORCE_POLL -o $(BUILD_DIR)/runtime-ownership/scheduler-poll $(OWNERSHIP_SCHED_SRCS) -lm
	$(BUILD_DIR)/runtime-ownership/scheduler-poll
	$(CC) $(ASAN_UBSAN_CFLAGS) -o $(BUILD_DIR)/runtime-ownership/scheduler-sanitize $(OWNERSHIP_SCHED_SRCS) -lm
	$(BUILD_DIR)/runtime-ownership/scheduler-sanitize
	$(CC) $(TEST_CFLAGS) -pthread -o $(BUILD_DIR)/runtime-ownership/mcp-stream tests/test_mcp_stream.c src/mcp_stream.c src/mcp_response.c src/json_fast.c
	$(BUILD_DIR)/runtime-ownership/mcp-stream
	$(CC) $(ASAN_UBSAN_CFLAGS) -pthread -o $(BUILD_DIR)/runtime-ownership/mcp-stream-sanitize tests/test_mcp_stream.c src/mcp_stream.c src/mcp_response.c src/json_fast.c
	$(BUILD_DIR)/runtime-ownership/mcp-stream-sanitize
	$(CC) $(TEST_CFLAGS) -D_DARWIN_C_SOURCE -D_GNU_SOURCE -ffunction-sections -fdata-sections -o $(BUILD_DIR)/runtime-ownership/mcp-stdio tests/mcp_stdio_ownership_fixture.c src/cancel_wait.c src/mcp_stream.c src/mcp_response.c src/http_pool.c src/json_util.c src/json_fast.c $(if $(filter Darwin,$(UNAME_S)),-Xlinker -dead_strip,-Xlinker --gc-sections) -lcurl -lm -lpthread
	$(BUILD_DIR)/runtime-ownership/mcp-stdio
	$(CC) $(TSAN_CFLAGS) -D_DARWIN_C_SOURCE -D_GNU_SOURCE -ffunction-sections -fdata-sections -o $(BUILD_DIR)/runtime-ownership/mcp-stdio-tsan tests/mcp_stdio_ownership_fixture.c src/cancel_wait.c src/mcp_stream.c src/mcp_response.c src/http_pool.c src/json_util.c src/json_fast.c $(if $(filter Darwin,$(UNAME_S)),-Xlinker -dead_strip,-Xlinker --gc-sections) -lcurl -lm -lpthread
	$(BUILD_DIR)/runtime-ownership/mcp-stdio-tsan

# Offline historical corpus replay links the same production objects and gate.
$(BUILD_DIR)/conversation_corpus_driver: tests/conversation_corpus_driver.c $(filter-out $(OBJ_DIR)/main.o,$(OBJS)) $(GSL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(RELEASE_LDFLAGS) $(LDLIBS)

.PHONY: test-context-fts-index
test-context-fts-index:
	$(CC) $(BASE_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/test_context_fts_index tests/test_context_fts_index.c -lsqlite3 -lm
	$(BUILD_DIR)/test_context_fts_index

.PHONY: test-tool-integrity-index
test-tool-integrity-index:
	$(CC) $(ASAN_UBSAN_CFLAGS) -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/test_tool_integrity_index tests/test_tool_integrity_index.c src/tool_integrity_index.c src/json_util.c src/json_fast.c -lcurl -lm
	$(BUILD_DIR)/test_tool_integrity_index

# Progressive real elapsed-time reliability ladder (explicit long-running target).
LONG_CYCLE_CORE_SRCS = tests/harness_long_cycles.c src/async_runtime.c src/async_join.c src/scheduler.c src/scheduler_lifetime.c src/scheduler_io.c src/event_loop.c src/event_wakeup.c
.PHONY: harness-long-cycle-build harness-long-cycles
harness-long-cycle-build:
	@mkdir -p $(BUILD_DIR)/long-cycles
	$(CC) $(TEST_CFLAGS) -pthread -o $(BUILD_DIR)/long-cycles/core $(LONG_CYCLE_CORE_SRCS)
	$(CC) $(TEST_CFLAGS) -pthread -DEV_FORCE_POLL -o $(BUILD_DIR)/long-cycles/core-poll $(LONG_CYCLE_CORE_SRCS)
	$(CC) $(TEST_CFLAGS) -D_DARWIN_C_SOURCE -D_GNU_SOURCE -ffunction-sections -fdata-sections -o $(BUILD_DIR)/long-cycles/mcp tests/mcp_long_cycles_fixture.c tests/cancel_wait_measured.c src/mcp_stream.c src/mcp_response.c src/http_pool.c src/json_util.c src/json_fast.c $(if $(filter Darwin,$(UNAME_S)),-Xlinker -dead_strip,-Xlinker --gc-sections) -lcurl -lm -lpthread
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/long-cycles/job-wait tests/long_job_wait.c src/shell_job_events.c src/json_fast.c
harness-long-cycles: $(TARGET) harness-long-cycle-build
	python3 tests/test_harness_long_cycles.py --binary ./$(TARGET) --output $(BUILD_DIR)/long-cycles/results

.PHONY: test-cancel-wait
test-cancel-wait:
	@mkdir -p $(BUILD_DIR)/long-cycles
	$(CC) $(TEST_CFLAGS) -pthread -o $(BUILD_DIR)/long-cycles/cancel tests/test_cancel_wait.c src/cancel_wait.c
	$(BUILD_DIR)/long-cycles/cancel
	$(CC) $(ASAN_UBSAN_CFLAGS) -pthread -o $(BUILD_DIR)/long-cycles/cancel-sanitize tests/test_cancel_wait.c src/cancel_wait.c
	$(BUILD_DIR)/long-cycles/cancel-sanitize
	$(CC) $(TSAN_CFLAGS) -pthread -o $(BUILD_DIR)/long-cycles/cancel-tsan tests/test_cancel_wait.c src/cancel_wait.c
	$(BUILD_DIR)/long-cycles/cancel-tsan

.PHONY: test_native_window_layout
test_native_window_layout:
	$(CC) $(ASAN_UBSAN_CFLAGS) -o $(BUILD_DIR)/$@ tests/test_native_window_layout.c src/native_window_layout.c
	$(BUILD_DIR)/$@

.PHONY: test-execution-plan test-execution-planner
test-execution-plan:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -Wall -Wextra -Werror -Iinclude -o $(BUILD_DIR)/test_execution_plan tests/test_execution_plan.c src/execution_plan.c src/cost_frontier.c src/task_profile.c src/json_util.c $(ASAN_LDFLAGS) -lm
	ASAN_OPTIONS='$(ASAN_RUNTIME_OPTIONS):halt_on_error=1' $(BUILD_DIR)/test_execution_plan

# Offline bounded planner integration/regressions, no model calls or workers.
test-execution-planner: test-execution-plan test-cost-frontier test-work-algebra test-goal-queue test-execution-contract test-gate-claims

test: test-execution-plan


.PHONY: test-interop-reliability
test-interop-reliability: $(TARGET) test-process-capture-lifecycle
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_agent_run_outcome tests/test_agent_run_outcome.c src/agent_run_outcome.c vendor/yyjson.c
	$(BUILD_DIR)/test_agent_run_outcome
	$(CC) $(TEST_CFLAGS) -o $(BUILD_DIR)/test_interop_signals tests/test_interop_signals.c src/agent_interop.c src/agent_run_outcome.c src/subscription_paths.c src/process_capture.c src/json_util.c vendor/yyjson.c
	$(BUILD_DIR)/test_interop_signals
	python3 tests/test_interop_reliability.py --binary ./$(TARGET)

# Offline production capture-parser regression: no providers or workers.
.PHONY: test-pixel-capture-parser
test-pixel-capture-parser:
	python3 tests/test_pixel_capture_parser.py

test: test-pixel-capture-parser

.PHONY: test-context-lifecycle
test-context-lifecycle:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_UBSAN_CFLAGS) -D_DARWIN_C_SOURCE -ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Xlinker -dead_strip,-Xlinker --gc-sections) -o $(BUILD_DIR)/test_context_lifecycle tests/test_context_lifecycle.c src/goal.c src/goal_queue.c src/context_archive.c src/input_budget.c src/vfs.c src/vecstore.c src/crypto.c src/context_eviction.c src/env_config.c src/llm.c src/tool_integrity_index.c src/json_util.c src/json_fast.c -lsqlite3 -lcurl -lpthread -lm
	$(BUILD_DIR)/test_context_lifecycle

# Public declaration implementations: offline native, classical fallback and
# sanitizer checks. Crypto wrappers are never substituted into legacy formats.
DECLARATION_COMMA := ,
DECLARATION_TEST_FLAGS = $(if $(filter 1,$(DECLARATION_SANITIZE)),$(filter-out -O3 -O2 -D_FORTIFY_SOURCE=2,$(TEST_CFLAGS)) -fsanitize=address$(DECLARATION_COMMA)undefined -fno-sanitize-recover=all,$(TEST_CFLAGS))
.PHONY: test-declarations test-declarations-sanitize
test-declarations: | $(BUILD_DIR)
	$(CC) $(DECLARATION_TEST_FLAGS) $(CRYPTO_PRIMITIVE_CFLAGS) -o $(BUILD_DIR)/test_crypto_primitives tests/test_crypto_primitives.c src/crypto_primitives.c src/crypto.c $(SODIUM_LIBS) $(shell pkg-config --libs --static liboqs 2>/dev/null) -lpthread
	$(BUILD_DIR)/test_crypto_primitives
	$(CC) $(DECLARATION_TEST_FLAGS) -DDSCO_CRYPTO_FORCE_XCHACHA=1 -o $(BUILD_DIR)/test_crypto_classical tests/test_crypto_primitives.c src/crypto_primitives.c src/crypto.c $(SODIUM_LIBS) -lpthread
	$(BUILD_DIR)/test_crypto_classical
	$(CC) $(DECLARATION_TEST_FLAGS) -UHAVE_LIBSODIUM -o $(BUILD_DIR)/test_crypto_portable tests/test_crypto_primitives.c src/crypto_primitives.c src/crypto.c -lpthread
	$(BUILD_DIR)/test_crypto_portable
	$(CC) $(DECLARATION_TEST_FLAGS) -o $(BUILD_DIR)/test_rollout tests/test_rollout.c src/rollout.c src/crypto.c src/json_util.c src/json_fast.c $(if $(filter Darwin,$(UNAME_S)),-Wl$(DECLARATION_COMMA)-dead_strip,-ffunction-sections -Wl$(DECLARATION_COMMA)--gc-sections) -lm -lpthread
	$(BUILD_DIR)/test_rollout
test-declarations-sanitize: | $(BUILD_DIR)
	$(MAKE) test-declarations DECLARATION_SANITIZE=1

# Stable native geometry and semantic turn ordinals; local fixtures only.
.PHONY: test-native-turns
test-native-turns: $(TARGET) test_native_motion test-context-lifecycle
	python3 tests/test_user_turn_counter_binary.py --binary "$(abspath $(TARGET))"
	python3 tests/test_user_turn_counter_binary.py --binary "$(abspath $(TARGET))" --native

.PHONY: test-model-catalog-pricing
test-model-catalog-pricing:
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Iinclude $$(pkg-config --cflags libcurl) tests/test_model_catalog_pricing.c src/model_pricing.c $$(pkg-config --libs libcurl) -lpthread -o $(BUILD_DIR)/test_model_catalog_pricing
	$(BUILD_DIR)/test_model_catalog_pricing
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_GNU_SOURCE -ffunction-sections -fdata-sections -Iinclude $$(pkg-config --cflags libcurl) tests/test_dynamic_catalog_pricing.c src/model_pricing.c src/json_util.c $$(pkg-config --libs libcurl) -lpthread -Wl,-dead_strip -o $(BUILD_DIR)/test_dynamic_catalog_pricing
	$(BUILD_DIR)/test_dynamic_catalog_pricing
