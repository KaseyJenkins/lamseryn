// tests/unit/test_logger_greatest.c
#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"
#include <stdlib.h>
#include <string.h>

#include "logger.h"

static void unset_env(const char *k) {
  unsetenv(k);
}

// Reset helper to a known baseline (LOG_WARN, all categories)
static void reset_logger(void) {
  log_set_level(LOG_WARN);
  log_set_categories(LOGC_ALL);
  log_set_thread_id(-1);
}

// ---- Tests ----
TEST t_init_from_env_defaults(void) {
  reset_logger();
  unset_env("LOG_LEVEL");
  unset_env("LOG_CATS");

  log_init_from_env();

  ASSERT_EQ(log_get_level(), LOG_WARN);
  ASSERT_EQ(log_get_categories(), LOGC_ALL);
  PASS();
}

TEST t_parse_level_and_cats(void) {
  reset_logger();

  setenv("LOG_LEVEL", "debug", 1);
  setenv("LOG_CATS", "http,io", 1);
  log_init_from_env();

  ASSERT_EQ(log_get_level(), LOG_DEBUG);
  unsigned cats = log_get_categories();
  ASSERT((cats & LOGC_HTTP) != 0);
  ASSERT((cats & LOGC_IO) != 0);
  PASS();
}

TEST t_should_emit_respects_level_and_mask(void) {
  reset_logger();
  log_set_level(LOG_INFO);
  log_set_categories(LOGC_HTTP); // only HTTP

  ASSERT(log_should_emit(LOG_INFO, LOGC_HTTP));
  ASSERT(!log_should_emit(LOG_DEBUG, LOGC_HTTP));
  ASSERT(!log_should_emit(LOG_INFO, LOGC_CORE));
  PASS();
}

TEST t_thread_id_roundtrip(void) {
  reset_logger();
  log_set_thread_id(42);
  ASSERT_EQ(log_get_thread_id(), 42);
  PASS();
}

SUITE(s_logger) {
  RUN_TEST(t_init_from_env_defaults);
  RUN_TEST(t_parse_level_and_cats);
  RUN_TEST(t_should_emit_respects_level_and_mask);
  RUN_TEST(t_thread_id_roundtrip);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_logger);
  GREATEST_MAIN_END();
}
