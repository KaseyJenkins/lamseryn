# Build app with strict warnings while keeping third-party builds lenient.

APP        := lamseryn
APP_GATES  := $(APP)_gates
SRC        := lamseryn.c src/net_server.c src/http_parser.c src/http_pipeline.c src/http_boundary.c src/request_handlers.c src/conn_store.c src/buffer_pool.c src/accept_controller.c src/timing_wheel.c src/url.c src/http_headers.c src/http1_limits.c src/req_arena_stats.c src/tx.c src/rx_stash.c src/rx_buffers.c src/conn_deadline.c src/worker_loop.c src/static_serve_utils.c src/conn_lifecycle.c src/http_range.c src/compression.c
SRC       += src/config_ini.c
SRC       += src/access_log.c
SRC       += src/tls.c
SRC_ITEST  := $(SRC) src/itest_echo.c

# Integration-test build uses smaller caps/timeouts.
APP_ITEST  := $(APP)_itest
TLS_ITEST_PORT ?= 18443
ENABLE_TLS_ITESTS ?= 1
PHASE1_GATE_PROFILE ?= ci-fast
PHASE1_GATE_THREADS ?= 1
PHASE1_GATE_PORT ?= 18086

# 1 = inline ring ops (default), 0 = out-of-line
RING_OPS_INLINE ?= 1
ifeq ($(RING_OPS_INLINE),0)
    SRC += src/ring_ops.c
endif

LOGGER_SRC := src/logger.c
LOGGER_HDR := include/logger.h

# Keep plain make targeting the primary app.
.DEFAULT_GOAL := all

# Base build directory.
BUILD_BASE := build

LIBUR_DIR  := third_party/liburing
LLHTTP_DIR := third_party/llhttp
INI_DIR    := third_party/inih
OPENSSL_SRC_DIR := third_party/openssl
OPENSSL_LOCAL_PREFIX := $(OPENSSL_SRC_DIR)/_install
ZLIB_DIR   := third_party/zlib
BROTLI_DIR := third_party/brotli

# liburing wiring:
# - If third_party/liburing exists, build/link that local copy.
# - Otherwise, link against system liburing (-luring).
# - In either mode, require liburing >= 2.4.
ifeq ($(wildcard $(LIBUR_DIR)/Makefile),)
LIBURING_MODE := system
LIBURING_CFLAGS :=
LIBURING_LIBS := -luring
LIBURING_DEPS :=
LIBURING_LINK_INPUTS :=
LIBURING_CHECK_DEPS := check-system-liburing-version
else
LIBURING_MODE := local
LIBURING_CFLAGS := -I"$(LIBUR_DIR)/src/include"
LIBURING_LIBS :=
LIBURING_DEPS := $(LIBUR_DIR)/src/liburing.a
LIBURING_LINK_INPUTS := "$(LIBUR_DIR)/src/liburing.a"
LIBURING_CHECK_DEPS := check-local-liburing-version
endif

CC         := gcc

CSTD ?= gnu11

# Compile-time logging ceiling.
LOG_LEVEL_CEILING ?= LOG_TRACE

# Compile-time instrumentation level: 0=none, 1=ops, 2=dev.
INSTRUMENTATION_LEVEL ?= 2

SANITIZE ?= none

ifeq ($(SANITIZE),asan)
	BUILD := $(BUILD_BASE)/asan
else
	BUILD := $(BUILD_BASE)
endif

# Sanitizer flags (compile + link)
SAN_CFLAGS  :=
SAN_LDFLAGS :=
ifeq ($(SANITIZE),asan)
	SAN_CFLAGS  += -fsanitize=address -fno-omit-frame-pointer -g
	SAN_LDFLAGS += -fsanitize=address
endif

# Base CFLAGS used everywhere
BASE_CFLAGS := -O2 -g -Wall -Wextra -Wvla -pipe -fno-plt -fno-omit-frame-pointer \
			   -I"include" -I"." \
			   $(LIBURING_CFLAGS) \
			   -I"$(LLHTTP_DIR)/include" \
			   -I"$(INI_DIR)" \
			   $(ZLIB_CFLAGS)

# Tool-specific flags.
TOOLS_CFLAGS := -O2 -g -Wall -Wextra -Wvla -std=$(CSTD)

TOOLS_BIN := $(BUILD)/bench_client

# App-specific flags. Extra -D knobs can be injected by profiles/CI.
APP_DEFS ?=

# Optional TLS/OpenSSL wiring:
# - ENABLE_TLS=0 (default): no TLS link deps; runtime behavior remains non-TLS.
# - ENABLE_TLS=1: define ENABLE_TLS and link against OpenSSL dynamically.
# - OPENSSL_PREFIX=/path: prefer that OpenSSL include/lib location.
# - If OPENSSL_PREFIX is unset and third_party/openssl/_install exists,
#   use that local install automatically.
ENABLE_TLS ?= 0
OPENSSL_PREFIX ?=

ifeq ($(strip $(OPENSSL_PREFIX)),)
ifneq ($(wildcard $(OPENSSL_LOCAL_PREFIX)/include/openssl/ssl.h),)
OPENSSL_PREFIX := $(OPENSSL_LOCAL_PREFIX)
endif
endif

TLS_CFLAGS :=
TLS_LDFLAGS :=
TLS_LDLIBS :=

ifeq ($(ENABLE_TLS),1)
TLS_CFLAGS += -DENABLE_TLS=1
TLS_LDLIBS += -lssl -lcrypto
else
TLS_CFLAGS += -DENABLE_TLS=0
endif

ifneq ($(strip $(OPENSSL_PREFIX)),)
TLS_CFLAGS += -I"$(OPENSSL_PREFIX)/include"
TLS_LDFLAGS += -L"$(OPENSSL_PREFIX)/lib" -Wl,-rpath,"$(OPENSSL_PREFIX)/lib"
endif

# zlib wiring:
# - If third_party/zlib/zlib.h exists (source tree present), build/link that local copy.
# - Otherwise, link against system zlib (-lz).
# zlib is a hard dependency for dynamic compression.
ZLIB_CFLAGS :=
ZLIB_LDFLAGS :=
ZLIB_LDLIBS :=
ZLIB_DEPS :=
ZLIB_LINK_INPUTS :=
ifneq ($(wildcard $(ZLIB_DIR)/zlib.h),)
ZLIB_CFLAGS      := -I"$(ZLIB_DIR)"
ZLIB_LDFLAGS     := -L"$(ZLIB_DIR)/_build"
ZLIB_LDLIBS      := -lz
ZLIB_DEPS        := $(ZLIB_DIR)/_build/libz.a
ZLIB_LINK_INPUTS := "$(ZLIB_DIR)/_build/libz.a"
else
ZLIB_LDLIBS := -lz
endif

# Brotli wiring (optional):
# - If third_party/brotli/c/include/brotli/encode.h exists (source tree present),
#   set BROTLI_DEPS so make builds _build/libbrotlienc.a automatically.
# - Otherwise, probe for system libbrotlienc via pkg-config.
# - If neither is available, brotli support is disabled.
BROTLI_CFLAGS      :=
BROTLI_LDFLAGS     :=
BROTLI_LDLIBS      :=
BROTLI_DEPS        :=
BROTLI_LINK_INPUTS :=
ifneq ($(wildcard $(BROTLI_DIR)/c/include/brotli/encode.h),)
BROTLI_CFLAGS      := -DHAVE_BROTLI -isystem "$(BROTLI_DIR)/c/include"
BROTLI_LDFLAGS     := -L"$(BROTLI_DIR)/_build"
BROTLI_LDLIBS      := -lbrotlienc -lbrotlicommon -lm
BROTLI_DEPS        := $(BROTLI_DIR)/_build/libbrotlienc.a
BROTLI_LINK_INPUTS := "$(BROTLI_DIR)/_build/libbrotlienc.a" "$(BROTLI_DIR)/_build/libbrotlicommon.a"
else
BROTLI_PKG := $(shell pkg-config --exists libbrotlienc 2>/dev/null && echo yes)
ifeq ($(BROTLI_PKG),yes)
BROTLI_CFLAGS  := -DHAVE_BROTLI $(shell pkg-config --cflags libbrotlienc)
BROTLI_LDLIBS  := $(shell pkg-config --libs libbrotlienc)
endif
endif

# Stamp file so APP_DEFS changes rebuild binaries.
APP_DEFS_STAMP := $(BUILD)/.app_defs

.PHONY: FORCE
FORCE:

APP_CFLAGS := $(BASE_CFLAGS) -std=$(CSTD) -Wshadow -Werror $(SAN_CFLAGS) \
              -DLOG_COMPILE_LEVEL=$(LOG_LEVEL_CEILING) \
              -DINSTRUMENTATION_LEVEL=$(INSTRUMENTATION_LEVEL) \
			  -DRING_OPS_INLINE=$(RING_OPS_INLINE) \
			  $(TLS_CFLAGS) \
			  $(ZLIB_CFLAGS) \
			  $(BROTLI_CFLAGS) \
			  $(APP_DEFS)

# llhttp-specific flags (no -Werror).
LLHTTP_CFLAGS := $(BASE_CFLAGS) -std=$(CSTD) -Wno-unused-parameter $(SAN_CFLAGS)

LDFLAGS    := $(SAN_LDFLAGS) -rdynamic $(TLS_LDFLAGS) $(ZLIB_LDFLAGS) $(BROTLI_LDFLAGS)
LDLIBS     := -lpthread $(TLS_LDLIBS) $(ZLIB_LDLIBS) $(BROTLI_LDLIBS)

STATIC_LIB_LLHTTP := $(BUILD)/libllhttp.a

LLHTTP_SRC := $(wildcard $(LLHTTP_DIR)/src/*.c)
LLHTTP_OBJ := $(patsubst $(LLHTTP_DIR)/src/%.c,$(BUILD)/llhttp/%.o,$(LLHTTP_SRC))

INI_OBJ := $(BUILD)/inih/ini.o

# Per-object app build: derive object lists from source lists.
APP_OBJ       := $(patsubst %.c,$(BUILD)/app/%.o,$(SRC))
LOGGER_OBJ    := $(patsubst %.c,$(BUILD)/app/%.o,$(LOGGER_SRC))

# Include generated dependency files (silently if absent).
-include $(APP_OBJ:.o=.d)
-include $(LOGGER_OBJ:.o=.d)

# $(LIBURING_DEPS) is order-only so the liburing sub-build (which regenerates
# compat.h via configure) finishes before we include its headers.
# $(ZLIB_DEPS) is order-only for the same reason (zlib.h may need configure output).
$(BUILD)/app/%.o: %.c $(APP_DEFS_STAMP) | $(LIBURING_DEPS) $(ZLIB_DEPS) $(BUILD)/app $(BUILD)/app/src
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -MMD -MP -c $< -o $@

.PHONY: all
all: $(BUILD)/$(APP)

.PHONY: gates
gates: $(BUILD)/$(APP_GATES)

.PHONY: pipeline_test
pipeline_test: $(BUILD)/pipeline_test

$(BUILD)/pipeline_test: tests/integration/pipeline_coalesce_test.c | $(BUILD)
	$(CC) $(TOOLS_CFLAGS) $(BROTLI_CFLAGS) -o $@ $<

.PHONY: bench
bench: $(TOOLS_BIN)

$(TOOLS_BIN): tools/bench/bench_client.c | $(BUILD)
	$(CC) $(TOOLS_CFLAGS) -o $@ $<

.PHONY: bench-sweep
bench-sweep: bench
	python3 tools/bench/run_bench_sweep.py

.PHONY: bench-managed
bench-managed: bench gates
	python3 tools/bench/run_bench_managed.py --build $(if $(ALLOW_ERRORS),--allow-errors,) $(if $(DURATION),--duration $(DURATION),)

.PHONY: itest
itest: all pipeline_test $(BUILD)/$(APP_ITEST)
	@ENABLE_EXTRA_ITESTS=1 bash tests/integration/run_integration_tests.sh "$(BUILD)/$(APP_ITEST)" "$(BUILD)/pipeline_test" && \
	if [ "$(ENABLE_TLS_ITESTS)" = "1" ]; then \
		$(MAKE) itest-tls; \
	fi

.PHONY: phase1-gate
phase1-gate:
	@$(MAKE) -B -C tests test && \
	$(MAKE) -B itest && \
	python3 tools/gates/run_timeout_gates.py --profile "$(PHASE1_GATE_PROFILE)" --threads "$(PHASE1_GATE_THREADS)" --port "$(PHASE1_GATE_PORT)"

.PHONY: itest-tls
itest-tls:
	@$(MAKE) -B ENABLE_TLS=1 $(BUILD)/$(APP_ITEST) && \
	OPENSSL_PREFIX="$(OPENSSL_PREFIX)" bash tests/integration/run_tls_integration_tests.sh "$(BUILD)/$(APP_ITEST)" "$(TLS_ITEST_PORT)"

$(BUILD)/$(APP): $(APP_DEFS_STAMP) $(APP_OBJ) $(LOGGER_OBJ) $(LOGGER_HDR) $(LIBURING_CHECK_DEPS) $(LIBURING_DEPS) $(ZLIB_DEPS) $(BROTLI_DEPS) $(STATIC_LIB_LLHTTP) $(INI_OBJ) | $(BUILD)
	$(CC) $(APP_CFLAGS) -o $@ $(APP_OBJ) $(LOGGER_OBJ) $(LIBURING_LINK_INPUTS) $(ZLIB_LINK_INPUTS) $(BROTLI_LINK_INPUTS) "$(STATIC_LIB_LLHTTP)" $(INI_OBJ) $(LDFLAGS) $(LDLIBS) $(LIBURING_LIBS)

$(BUILD)/$(APP_GATES): $(APP_DEFS_STAMP) $(APP_OBJ) $(LOGGER_OBJ) $(LOGGER_HDR) $(LIBURING_CHECK_DEPS) $(LIBURING_DEPS) $(ZLIB_DEPS) $(BROTLI_DEPS) $(STATIC_LIB_LLHTTP) $(INI_OBJ) | $(BUILD)
	$(CC) $(APP_CFLAGS) -o $@ $(APP_OBJ) $(LOGGER_OBJ) $(LIBURING_LINK_INPUTS) $(ZLIB_LINK_INPUTS) $(BROTLI_LINK_INPUTS) "$(STATIC_LIB_LLHTTP)" $(INI_OBJ) $(LDFLAGS) $(LDLIBS) $(LIBURING_LIBS)

# itest uses extra -D flags that differ from the main build, so it compiles
# from source rather than reusing main app objects.
$(BUILD)/$(APP_ITEST): $(APP_DEFS_STAMP) $(SRC_ITEST) $(LOGGER_SRC) $(LOGGER_HDR) $(LIBURING_CHECK_DEPS) $(LIBURING_DEPS) $(ZLIB_DEPS) $(BROTLI_DEPS) $(STATIC_LIB_LLHTTP) $(INI_OBJ) | $(BUILD)
	$(CC) $(APP_CFLAGS) -o $@ $(SRC_ITEST) $(LOGGER_SRC) \
		-DBODY_TIMEOUT_MS=200 -DMAX_BODY_BYTES=32 -DENABLE_ITEST_ECHO=1 -DACCESS_LOG_ENABLE_TEST_HOOKS=1 \
		$(LIBURING_LINK_INPUTS) $(ZLIB_LINK_INPUTS) $(BROTLI_LINK_INPUTS) "$(STATIC_LIB_LLHTTP)" $(INI_OBJ) $(LDFLAGS) $(LDLIBS) $(LIBURING_LIBS)

$(APP_DEFS_STAMP): FORCE | $(BUILD)
	@tmp="$@.tmp"; \
	printf '%s\n' "$(APP_DEFS)" > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$@" "$$tmp"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
	fi

$(LIBUR_DIR)/src/liburing.a:
	$(MAKE) -C "$(LIBUR_DIR)"

ZLIB_SRC := $(wildcard $(ZLIB_DIR)/*.c)

$(ZLIB_DIR)/_build/libz.a: $(ZLIB_SRC)
	@if [ ! -f "$(ZLIB_DIR)/zlib.h" ]; then \
		echo "zlib source tree not found at $(ZLIB_DIR)"; exit 1; \
	fi
	@mkdir -p "$(ZLIB_DIR)/_build"
	@for f in $(ZLIB_SRC); do \
		$(CC) -O2 -I"$(ZLIB_DIR)" -DHAVE_UNISTD_H -c "$$f" -o "$(ZLIB_DIR)/_build/$$(basename $$f .c).o"; \
	done
	ar rcs "$(ZLIB_DIR)/_build/libz.a" $(ZLIB_DIR)/_build/*.o

BROTLI_COMMON_SRC := $(wildcard $(BROTLI_DIR)/c/common/*.c)
BROTLI_ENC_SRC    := $(wildcard $(BROTLI_DIR)/c/enc/*.c)
BROTLI_DEC_SRC    := $(wildcard $(BROTLI_DIR)/c/dec/*.c)
BROTLI_BUILD_DIR  := $(BROTLI_DIR)/_build

$(BROTLI_DIR)/_build/libbrotlienc.a: $(BROTLI_COMMON_SRC) $(BROTLI_ENC_SRC) $(BROTLI_DEC_SRC)
	@if [ ! -f "$(BROTLI_DIR)/c/include/brotli/encode.h" ]; then \
		echo "Brotli source tree not found at $(BROTLI_DIR)"; exit 1; \
	fi
	@mkdir -p "$(BROTLI_BUILD_DIR)/common" "$(BROTLI_BUILD_DIR)/enc" "$(BROTLI_BUILD_DIR)/dec"
	@for f in $(BROTLI_COMMON_SRC); do \
		$(CC) -O2 -I"$(BROTLI_DIR)/c/include" -c "$$f" -o "$(BROTLI_BUILD_DIR)/common/$$(basename $$f .c).o"; \
	done
	@for f in $(BROTLI_ENC_SRC); do \
		$(CC) -O2 -I"$(BROTLI_DIR)/c/include" -c "$$f" -o "$(BROTLI_BUILD_DIR)/enc/$$(basename $$f .c).o"; \
	done
	@for f in $(BROTLI_DEC_SRC); do \
		$(CC) -O2 -I"$(BROTLI_DIR)/c/include" -c "$$f" -o "$(BROTLI_BUILD_DIR)/dec/$$(basename $$f .c).o"; \
	done
	ar rcs "$(BROTLI_BUILD_DIR)/libbrotlicommon.a" $(BROTLI_BUILD_DIR)/common/*.o
	ar rcs "$(BROTLI_BUILD_DIR)/libbrotlienc.a"    $(BROTLI_BUILD_DIR)/enc/*.o
	ar rcs "$(BROTLI_BUILD_DIR)/libbrotlidec.a"    $(BROTLI_BUILD_DIR)/dec/*.o

$(BROTLI_DIR)/_build/libbrotlidec.a: $(BROTLI_DIR)/_build/libbrotlienc.a

.PHONY: check-system-liburing-version
check-system-liburing-version: | $(BUILD)
	@tmp_c="$(BUILD)/.liburing_check.c"; \
	tmp_bin="$(BUILD)/.liburing_check.bin"; \
	printf '%s\n' \
		'#include <liburing.h>' \
		'#ifndef IO_URING_VERSION_MAJOR' \
		'#error "liburing version macros not available"' \
		'#endif' \
		'#ifndef IO_URING_VERSION_MINOR' \
		'#error "liburing version macros not available"' \
		'#endif' \
		'#if (IO_URING_VERSION_MAJOR < 2) || ((IO_URING_VERSION_MAJOR == 2) && (IO_URING_VERSION_MINOR < 4))' \
		'#error "liburing >= 2.4 required"' \
		'#endif' \
		'int main(void) {' \
		'  return 0;' \
		'}' > "$$tmp_c"; \
	if ! $(CC) "$$tmp_c" -o "$$tmp_bin" -luring >/dev/null 2>&1; then \
		detected_ver="$$(printf '%s\n' '#include <liburing.h>' | $(CC) -E -dM -x c - 2>/dev/null | awk '$$2=="IO_URING_VERSION_MAJOR"{maj=$$3} $$2=="IO_URING_VERSION_MINOR"{min=$$3} END{if(maj != "" && min != "") printf "%s.%s", maj, min; else printf "unknown (version macros unavailable)"}')"; \
		if [ "$$detected_ver" = "unknown (version macros unavailable)" ]; then \
			pkg_ver="$$(dpkg-query -W -f='$${Version}' liburing-dev 2>/dev/null || true)"; \
			if [ -z "$$pkg_ver" ]; then \
				pkg_ver="$$(dpkg-query -W -f='$${Version}' liburing2 2>/dev/null || true)"; \
			fi; \
			if [ -n "$$pkg_ver" ]; then \
				detected_ver="package $$pkg_ver (header macros unavailable)"; \
			fi; \
		fi; \
		echo "Error: system liburing is missing or too old (need liburing >= 2.4)."; \
		echo "Detected system liburing version: $$detected_ver"; \
		echo "Install/upgrade system liburing-dev to >= 2.4, or vendor third_party/liburing."; \
		rm -f "$$tmp_c" "$$tmp_bin"; \
		exit 1; \
	fi; \
	rm -f "$$tmp_c" "$$tmp_bin"

.PHONY: check-local-liburing-version
check-local-liburing-version: $(LIBUR_DIR)/src/liburing.a | $(BUILD)
	@tmp_c="$(BUILD)/.liburing_local_check.c"; \
	tmp_obj="$(BUILD)/.liburing_local_check.o"; \
	printf '%s\n' \
		'#include <liburing.h>' \
		'#ifndef IO_URING_VERSION_MAJOR' \
		'#error "liburing version macros not available"' \
		'#endif' \
		'#ifndef IO_URING_VERSION_MINOR' \
		'#error "liburing version macros not available"' \
		'#endif' \
		'#if (IO_URING_VERSION_MAJOR < 2) || ((IO_URING_VERSION_MAJOR == 2) && (IO_URING_VERSION_MINOR < 4))' \
		'#error "liburing >= 2.4 required"' \
		'#endif' \
		'int main(void) {' \
		'  return 0;' \
		'}' > "$$tmp_c"; \
	if ! $(CC) -I"$(LIBUR_DIR)/src/include" -c "$$tmp_c" -o "$$tmp_obj" >/dev/null 2>&1; then \
		detected_ver="$$(printf '%s\n' '#include <liburing.h>' | $(CC) -I"$(LIBUR_DIR)/src/include" -E -dM -x c - 2>/dev/null | awk '$$2=="IO_URING_VERSION_MAJOR"{maj=$$3} $$2=="IO_URING_VERSION_MINOR"{min=$$3} END{if(maj != "" && min != "") printf "%s.%s", maj, min; else printf "unknown (version macros unavailable)"}')"; \
		echo "Error: vendored third_party/liburing is too old (need liburing >= 2.4)."; \
		echo "Detected vendored liburing version: $$detected_ver"; \
		echo "Update third_party/liburing to >= 2.4, or remove it to use a compatible system liburing-dev."; \
		rm -f "$$tmp_c" "$$tmp_obj"; \
		exit 1; \
	fi; \
	rm -f "$$tmp_c" "$$tmp_obj"

$(STATIC_LIB_LLHTTP): $(LLHTTP_OBJ) | $(BUILD)
	@mkdir -p "$(BUILD)"
	ar rcs $@ $(LLHTTP_OBJ)

$(BUILD)/llhttp/%.o: $(LLHTTP_DIR)/src/%.c | $(BUILD)/llhttp
	$(CC) $(LLHTTP_CFLAGS) -c $< -o $@

$(INI_OBJ): $(INI_DIR)/ini.c | $(BUILD)/inih
	$(CC) $(LLHTTP_CFLAGS) -c $< -o $@

$(BUILD)/llhttp:
	mkdir -p "$(BUILD)/llhttp"

$(BUILD)/inih:
	mkdir -p "$(BUILD)/inih"

$(BUILD)/app:
	mkdir -p "$(BUILD)/app"

$(BUILD)/app/src:
	mkdir -p "$(BUILD)/app/src"

$(BUILD):
	mkdir -p "$(BUILD)"

.PHONY: clean
clean:
	$(MAKE) -C "$(LIBUR_DIR)" clean || true
	rm -rf "$(BUILD_BASE)"

.PHONY: brotli-local brotli-local-clean
brotli-local: $(BROTLI_DIR)/_build/libbrotlienc.a
	@echo "Local brotli built at $(BROTLI_DIR)/_build/"
	@echo "Rebuild lamseryn with: make -j$$(nproc)"

brotli-local-clean:
	rm -rf "$(BROTLI_DIR)/_build"

.PHONY: zlib-local zlib-local-clean
zlib-local: $(ZLIB_DIR)/_build/libz.a
	@echo "Local zlib built at $(ZLIB_DIR)/_build/"
	@echo "Rebuild lamseryn with: make -j$$(nproc)"

zlib-local-clean:
	rm -rf "$(ZLIB_DIR)/_build"

.PHONY: openssl-local openssl-local-clean
openssl-local:
	@if [ ! -x "$(OPENSSL_SRC_DIR)/Configure" ]; then \
		echo "OpenSSL source tree not found at $(OPENSSL_SRC_DIR)"; \
		exit 1; \
	fi
	cd "$(OPENSSL_SRC_DIR)" && \
	./Configure --prefix="$(abspath $(OPENSSL_LOCAL_PREFIX))" --libdir=lib shared linux-x86_64
	$(MAKE) -C "$(OPENSSL_SRC_DIR)" -j$$(nproc)
	$(MAKE) -C "$(OPENSSL_SRC_DIR)" install_sw
	@echo "Local OpenSSL installed to $(OPENSSL_LOCAL_PREFIX)"
	@echo "Build with: make ENABLE_TLS=1 OPENSSL_PREFIX=$(OPENSSL_LOCAL_PREFIX) -j4"

openssl-local-clean:
	$(MAKE) -C "$(OPENSSL_SRC_DIR)" clean || true
	rm -rf "$(OPENSSL_LOCAL_PREFIX)"

# Convenience targets for compile-time log ceilings
.PHONY: release dev debug trace
release:
	$(MAKE) LOG_LEVEL_CEILING=LOG_WARN all

dev:
	$(MAKE) LOG_LEVEL_CEILING=LOG_INFO all

debug:
	$(MAKE) LOG_LEVEL_CEILING=LOG_DEBUG all

trace:
	$(MAKE) LOG_LEVEL_CEILING=LOG_TRACE all

# Convenience targets for instrumentation level
.PHONY: instr-none instr-ops instr-dev
instr-none:
	$(MAKE) INSTRUMENTATION_LEVEL=0 all

instr-ops:
	$(MAKE) INSTRUMENTATION_LEVEL=1 all

instr-dev:
	$(MAKE) INSTRUMENTATION_LEVEL=2 all

# Convenience target for ASAN builds
.PHONY: asan
asan:
	$(MAKE) SANITIZE=asan all
