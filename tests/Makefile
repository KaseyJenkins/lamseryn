# Tests build with real llhttp while keeping the root Makefile untouched.

CC   := gcc
CSTD ?= gnu11

# Include paths: project root, include/, and third-party deps.
CFLAGS := -O2 -g -Wall -Wextra -Wvla -pipe -fno-plt -fno-omit-frame-pointer \
          -ffunction-sections -fdata-sections \
          -std=$(CSTD) \
          -I .. \
          -I ../include \
		  -I ../third_party/inih \
          -I ../third_party/llhttp/include \
          -I ../third_party/liburing/src/include \
          -Wno-unused-parameter

# Link-time GC drops unreferenced functions.
LDFLAGS := -Wl,--gc-sections
LDLIBS  := -lpthread

# liburing static library.
LIBUR_DIR := ../third_party/liburing
LIBUR_A   := $(LIBUR_DIR)/src/liburing.a

# liburing wiring:
# - If vendored liburing exists, build/link that local copy.
# - Otherwise, link against system liburing (-luring).
ifeq ($(wildcard $(LIBUR_DIR)/Makefile),)
LIBURING_DEPS :=
LIBURING_LIBS := -luring
else
LIBURING_DEPS := $(LIBUR_A)
LIBURING_LIBS :=
endif

# llhttp-specific warning leniency.
LLHTTP_CFLAGS := -Wno-unused-parameter

BUILD := build

# Unit under test plus parser and llhttp.
UUT        := ../src/http_pipeline.c
PARSER     := ../src/http_parser.c
PARSER_DEPS := ../src/url.c ../src/http_headers.c ../src/http1_limits.c ../src/req_arena_stats.c
LLHTTP_DIR := ../third_party/llhttp
LLHTTP_SRC := $(wildcard $(LLHTTP_DIR)/src/*.c)

# Explicit test binaries.
TEST_BIN_REAL           := $(BUILD)/test_http_pipeline_real
TEST_BIN_GREATEST       := $(BUILD)/test_http_pipeline_greatest
TEST_BIN_STORE          := $(BUILD)/test_conn_store_greatest
TEST_BIN_HANDLERS       := $(BUILD)/test_request_handlers_greatest
TEST_BIN_WHEEL          := $(BUILD)/test_timing_wheel_greatest
TEST_BIN_BUFFERPOOL     := $(BUILD)/test_buffer_pool_greatest
TEST_BIN_RINGOPS        := $(BUILD)/test_ring_ops_greatest
TEST_BIN_ACCEPT         := $(BUILD)/test_accept_controller_greatest
TEST_BIN_LOGGER         := $(BUILD)/test_logger_greatest
TEST_BIN_BOUNDARY       := $(BUILD)/test_http_boundary_greatest
TEST_BIN_URL            := $(BUILD)/test_url_greatest
TEST_BIN_HTTP_HEADERS   := $(BUILD)/test_http_headers_greatest
TEST_BIN_HTTP1_LIMITS   := $(BUILD)/test_http1_limits_greatest
TEST_BIN_HTTP_PARSER_EXPECT := $(BUILD)/test_http_parser_expect_greatest
TEST_BIN_HTTP_PARSER_SMUGGLING := $(BUILD)/test_http_parser_smuggling_greatest
TEST_BIN_REQ_ARENA      := $(BUILD)/test_req_arena_greatest
TEST_BIN_REQ_ARENA_CACHE := $(BUILD)/test_req_arena_cache_greatest
TEST_BIN_CONFIG_INI     := $(BUILD)/test_config_ini_greatest
TEST_BIN_TX             := $(BUILD)/test_tx_greatest
TEST_BIN_ACCESS_LOG     := $(BUILD)/test_access_log_greatest
TEST_BIN_CONN_LIFECYCLE := $(BUILD)/test_conn_lifecycle_greatest

.PHONY: all
all: $(TEST_BIN_REAL) \
     $(TEST_BIN_GREATEST) \
     $(TEST_BIN_STORE) \
     $(TEST_BIN_HANDLERS) \
     $(TEST_BIN_WHEEL) \
     $(TEST_BIN_BUFFERPOOL) \
     $(TEST_BIN_RINGOPS) \
     $(TEST_BIN_ACCEPT) \
	$(TEST_BIN_LOGGER) \
	$(TEST_BIN_BOUNDARY) \
	$(TEST_BIN_URL) \
	$(TEST_BIN_HTTP_HEADERS) \
	$(TEST_BIN_HTTP1_LIMITS) \
	$(TEST_BIN_HTTP_PARSER_EXPECT) \
	$(TEST_BIN_HTTP_PARSER_SMUGGLING) \
	$(TEST_BIN_REQ_ARENA) \
	$(TEST_BIN_REQ_ARENA_CACHE) \
	$(TEST_BIN_CONFIG_INI) \
	$(TEST_BIN_TX) \
	$(TEST_BIN_ACCESS_LOG) \
	$(TEST_BIN_CONN_LIFECYCLE)

$(BUILD):
	@mkdir -p $(BUILD)

# Build liburing so tests can link against it.
$(LIBUR_A):
	$(MAKE) -C $(LIBUR_DIR)

$(TEST_BIN_REAL): unit/test_http_pipeline_real.c $(UUT) $(PARSER) $(PARSER_DEPS) $(LLHTTP_SRC) ../src/logger.c | $(BUILD)
	$(CC) $(CFLAGS) $(LLHTTP_CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_GREATEST): unit/test_http_pipeline_greatest.c $(UUT) $(PARSER) $(PARSER_DEPS) $(LLHTTP_SRC) ../src/logger.c | $(BUILD)
	$(CC) $(CFLAGS) $(LLHTTP_CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_STORE): unit/test_conn_store_greatest.c ../src/conn_store.c ../src/logger.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_HANDLERS): unit/test_request_handlers_greatest.c ../src/request_handlers.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^ $(LDFLAGS)

$(TEST_BIN_WHEEL): unit/test_timing_wheel_greatest.c ../src/timing_wheel.c ../src/request_handlers.c $(LIBURING_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^ $(LDFLAGS) $(LDLIBS) $(LIBURING_LIBS)

$(TEST_BIN_BUFFERPOOL): unit/test_buffer_pool_greatest.c ../src/buffer_pool.c ../src/logger.c $(LIBURING_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^ $(LDFLAGS) $(LDLIBS) $(LIBURING_LIBS)

$(TEST_BIN_RINGOPS): unit/test_ring_ops_greatest.c $(LIBURING_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^ $(LDFLAGS) $(LDLIBS) $(LIBURING_LIBS)

$(TEST_BIN_ACCEPT): unit/test_accept_controller_greatest.c ../src/accept_controller.c ../src/logger.c $(LIBURING_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^ $(LDFLAGS) $(LDLIBS) $(LIBURING_LIBS)

$(TEST_BIN_LOGGER): unit/test_logger_greatest.c ../src/logger.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_BOUNDARY): unit/test_http_boundary_greatest.c ../src/http_boundary.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_URL): unit/test_url_greatest.c ../src/url.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_HTTP_HEADERS): unit/test_http_headers_greatest.c ../src/http_headers.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_HTTP1_LIMITS): unit/test_http1_limits_greatest.c ../src/http1_limits.c $(PARSER) $(PARSER_DEPS) $(LLHTTP_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LLHTTP_CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_HTTP_PARSER_EXPECT): unit/test_http_parser_expect_greatest.c $(UUT) $(PARSER) $(PARSER_DEPS) $(LLHTTP_SRC) ../src/logger.c | $(BUILD)
	$(CC) $(CFLAGS) $(LLHTTP_CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_HTTP_PARSER_SMUGGLING): unit/test_http_parser_smuggling_greatest.c $(UUT) $(PARSER) $(PARSER_DEPS) $(LLHTTP_SRC) ../src/logger.c | $(BUILD)
	$(CC) $(CFLAGS) $(LLHTTP_CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_REQ_ARENA): unit/test_req_arena_greatest.c ../src/req_arena_stats.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_CONFIG_INI): unit/test_config_ini_greatest.c ../src/config_ini.c ../src/logger.c ../third_party/inih/ini.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_TX): unit/test_tx_greatest.c ../src/tx.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_REQ_ARENA_CACHE): unit/test_req_arena_cache_greatest.c ../src/req_arena_stats.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^

$(TEST_BIN_ACCESS_LOG): unit/test_access_log_greatest.c ../src/access_log.c ../src/logger.c | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN_CONN_LIFECYCLE): unit/test_conn_lifecycle_greatest.c ../src/conn_lifecycle.c ../src/conn_store.c ../src/req_arena_stats.c ../src/logger.c $(LIBURING_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -I vendor -o $@ $^ $(LDFLAGS) $(LDLIBS) $(LIBURING_LIBS)

TESTS ?= $(wildcard $(BUILD)/test_*)
TEST_FLAGS ?= -v

.PHONY: test
test: all
	@if [ -z "$(TESTS)" ]; then \
		echo "No tests found in $(BUILD)"; exit 0; \
	fi; \
	rc=0; \
	for t in $(TESTS); do \
		if [ -x "$$t" ]; then \
			printf "== %s ==\n" "$$t"; \
			"$$t" $(TEST_FLAGS) || rc=1; \
			echo; \
		fi; \
	done; \
	exit $$rc

test-list: all
	@$(MAKE) test TEST_FLAGS='-l'

test-fast: all
	@$(MAKE) test TEST_FLAGS='-v -f'

.PHONY: ls-tests
ls-tests: all
	@printf "Discovered tests (if any):\n"; ls -l $(BUILD)/test_* 2>/dev/null || echo "(none)"

.PHONY: clean
clean:
	rm -rf $(BUILD)