#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"
#include "include/worker_ctx.h"
#include "include/config.h"
#include "include/request_handlers.h"
#include "include/timing_wheel.h"
#include "include/conn.h"
#include "include/macros.h"

// timing_wheel.c uses these symbols in tw_process_tick.

static int g_close_calls = 0;
static int g_close_last_fd = -1;

int schedule_or_sync_close(struct worker_ctx *w, int fd) {
  (void)w;
  g_close_calls++;
  g_close_last_fd = fd;
  // Model synchronous close completion.
  return 1;
}

// request_handlers.c expects these response buffers to be provided by the main TU;
// in unit tests we define minimal ones to satisfy the externs.
const char RESP_OK_CLOSE[] = "OKC";
const size_t RESP_OK_CLOSE_len = sizeof(RESP_OK_CLOSE) - 1;
const char RESP_OK_KA[] = "OKK";
const size_t RESP_OK_KA_len = sizeof(RESP_OK_KA) - 1;
const char RESP_400[] = "B4";
const size_t RESP_400_len = sizeof(RESP_400) - 1;
const char RESP_403[] = "B403";
const size_t RESP_403_len = sizeof(RESP_403) - 1;
const char RESP_404[] = "B404";
const size_t RESP_404_len = sizeof(RESP_404) - 1;
const char RESP_405[] = "B405";
const size_t RESP_405_len = sizeof(RESP_405) - 1;
const char RESP_413[] = "H413";
const size_t RESP_413_len = sizeof(RESP_413) - 1;
const char RESP_431[] = "H431";
const size_t RESP_431_len = sizeof(RESP_431) - 1;
const char RESP_501[] = "H501";
const size_t RESP_501_len = sizeof(RESP_501) - 1;
const char RESP_408[] = "H408";
const size_t RESP_408_len = sizeof(RESP_408) - 1;
const char RESP_500[] = "H500";
const size_t RESP_500_len = sizeof(RESP_500) - 1;
#if ENABLE_OVERLOAD_503
const char RESP_503[] = "H503";
const size_t RESP_503_len = sizeof(RESP_503) - 1;
#endif

static struct worker_ctx w;

static void init_wheel(uint32_t slots, uint32_t tick_ms, uint64_t now) {
  memset(&w, 0, sizeof(w));
  tw_init(&w, slots, tick_ms, now);
}

static struct conn *make_conn_base(int fd) {
  struct conn *c = (struct conn *)calloc(1, sizeof(*c));
  c->fd = fd;
  c->refcnt = 1;
  return c;
}

TEST t_ka_idle_deadline(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(1);
  c->dl.ka_idle = 1;
  c->h1.parser_bytes = 0;
  c->dl.last_active_ms = 2000;
  tw_reschedule(&w, c, 2000);
  ASSERT_EQ(c->dl.deadline_kind, DK_KA_IDLE);
  ASSERT_EQ(c->dl.deadline_ms, c->dl.last_active_ms + IDLE_CLOSE_MS);
  ASSERT_EQ(c->dl.deadline_active, 1);
  free(c);
  PASS();
}

TEST t_initial_idle_deadline(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(2);
  // Initial idle: !ka_idle, !headers_done, parser_bytes == 0.
  c->dl.ka_idle = 0;
  c->h1.headers_done = 0;
  c->h1.parser_bytes = 0;
  c->dl.last_active_ms = 3000;
  tw_reschedule(&w, c, 3000);
  ASSERT_EQ(c->dl.deadline_kind, DK_INITIAL_IDLE);
  ASSERT_EQ(c->dl.deadline_ms, c->dl.last_active_ms + INITIAL_IDLE_TIMEOUT_MS);
  free(c);
  PASS();
}

TEST t_header_in_progress_deadline(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(3);
  // Header in progress: !ka_idle, !headers_done, parser_bytes > 0.
  c->dl.ka_idle = 0;
  c->h1.headers_done = 0;
  c->h1.parser_bytes = 17;
  c->dl.header_start_ms = 4000;
  tw_reschedule(&w, c, 4100);
  ASSERT_EQ(c->dl.deadline_kind, DK_HEADER_TIMEOUT);
  ASSERT_EQ(c->dl.deadline_ms, c->dl.header_start_ms + HEADER_TIMEOUT_MS);
  free(c);
  PASS();
}

TEST t_body_in_progress_deadline(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(33);
  c->dl.ka_idle = 0;
  c->h1.headers_done = 1;
  c->h1.message_done = 0;
  c->dl.last_active_ms = 7000;
  tw_reschedule(&w, c, 7000);
  ASSERT_EQ(c->dl.deadline_kind, DK_BODY_TIMEOUT);
  ASSERT_EQ(c->dl.deadline_ms, c->dl.last_active_ms + BODY_TIMEOUT_MS);
  free(c);
  PASS();
}

TEST t_write_in_progress_deadline(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(99);
  c->dl.ka_idle = 0;
  c->h1.headers_done = 1;
  c->h1.message_done = 1;
  c->dl.last_active_ms = 8000;
  c->dl.write_start_ms = 8000;

  static const char dummy[] = "abcdefghij";
  c->tx.write_buf = dummy;
  c->tx.write_len = sizeof(dummy) - 1;
  c->tx.write_off = 0;

  tw_reschedule(&w, c, 8000);
  ASSERT_EQ(c->dl.deadline_kind, DK_WRITE_TIMEOUT);
  ASSERT_EQ(c->dl.deadline_ms, c->dl.write_start_ms + WRITE_TIMEOUT_MS);
  free(c);
  PASS();
}

TEST t_draining_deadline(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(4);
  c->dl.draining = 1;
  c->dl.drain_deadline_ms = 5000;
  tw_reschedule(&w, c, 4500);
  ASSERT_EQ(c->dl.deadline_kind, DK_DRAIN);
  ASSERT_EQ(c->dl.deadline_ms, 5000);
  free(c);
  PASS();
}

TEST t_none_or_closing_no_deadline(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(5);
  c->dl.closing = 1;
  tw_reschedule(&w, c, 6000);
  ASSERT_EQ(c->dl.deadline_kind, DK_NONE);
  ASSERT_EQ((int)c->dl.deadline_active, 0);
  free(c);
  PASS();
}

TEST t_process_tick_expires_ka_idle_closes(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(6);

  c->dl.ka_idle = 1;
  c->h1.parser_bytes = 0;
  c->dl.last_active_ms = 0;

  // Arm such that deadline == now, placing it in the current slot.
  tw_reschedule(&w, c, IDLE_CLOSE_MS);
  ASSERT_EQ(c->dl.deadline_kind, DK_KA_IDLE);
  ASSERT_EQ((int)c->dl.deadline_active, 1);

  g_close_calls = 0;
  g_close_last_fd = -1;

  tw_process_tick(&w, IDLE_CLOSE_MS);

  ASSERT_EQ(g_close_calls, 1);
  ASSERT_EQ(g_close_last_fd, 6);
  ASSERT_EQ((int)c->dl.deadline_active, 0);
  free(c);
  PASS();
}

static int open_ring_or_skip(struct io_uring *ring, unsigned entries) {
  int rc = io_uring_queue_init(entries, ring, 0);
  if (rc < 0) {
    GREATEST_SKIPm("io_uring_queue_init failed; kernel/lib not available");
    return -1;
  }
  return 0;
}

static void close_ring(struct io_uring *ring) {
  io_uring_queue_exit(ring);
}

TEST t_process_tick_header_timeout_stages_408_send(void) {
  memset(&w, 0, sizeof(w));
  if (open_ring_or_skip(&w.ring, 8) < 0) {
    PASS();
  }
  tw_init(&w, 64, TW_TICK_MS, 1000);

  struct conn *c = make_conn_base(7);
  c->dl.ka_idle = 0;
  c->h1.headers_done = 0;
  c->h1.parser_bytes = 1;
  c->dl.header_start_ms = 0;

  // Arm such that deadline == now, placing it in the current slot.
  tw_reschedule(&w, c, HEADER_TIMEOUT_MS);
  ASSERT_EQ(c->dl.deadline_kind, DK_HEADER_TIMEOUT);
  ASSERT_EQ((int)c->dl.deadline_active, 1);

  g_close_calls = 0;
  g_close_last_fd = -1;

  tw_process_tick(&w, HEADER_TIMEOUT_MS);

  ASSERT_EQ(g_close_calls, 0);
  ASSERT_EQ(c->tx.resp_kind, RK_408);
  ASSERT_EQ((int)c->dl.closing, 1);
  ASSERT_EQ((int)w.need_submit, 1);
  ASSERT(w.sqes_pending >= 1);
  ASSERT(c->tx.write_buf != NULL);
  ASSERT(c->tx.write_len > 0);
  ASSERT_EQ((int)c->dl.deadline_active, 0);
  ASSERT_EQ((int)c->refcnt, 2);

  free(c);
  close_ring(&w.ring);
  PASS();
}

TEST t_process_tick_reinserts_not_yet_expired_deadline(void) {
  init_wheel(64, TW_TICK_MS, 1000);
  struct conn *c = make_conn_base(8);

  c->dl.ka_idle = 1;
  c->h1.parser_bytes = 0;
  c->dl.last_active_ms = 1000;

  uint64_t deadline_ms = c->dl.last_active_ms + IDLE_CLOSE_MS;
  uint64_t now_ms = deadline_ms - 1; // not yet expired, but in current tick slot

  tw_reschedule(&w, c, now_ms);
  ASSERT_EQ(c->dl.deadline_kind, DK_KA_IDLE);
  ASSERT_EQ((int)c->dl.deadline_active, 1);
  ASSERT_EQ((int)c->link.tw_slot, 0);

  g_close_calls = 0;
  g_close_last_fd = -1;

  tw_process_tick(&w, now_ms);

  ASSERT_EQ(g_close_calls, 0);
  ASSERT_EQ((int)c->dl.deadline_active, 1);
  ASSERT_EQ((int)c->link.tw_slot, 1);
  ASSERT_EQ(w.tw_buckets[0], NULL);
  ASSERT_EQ(w.tw_buckets[1], c);

  // clean up
  tw_cancel(&w, c);
  free(c);
  PASS();
}

SUITE(s_timing_wheel) {
  RUN_TEST(t_ka_idle_deadline);
  RUN_TEST(t_initial_idle_deadline);
  RUN_TEST(t_header_in_progress_deadline);
  RUN_TEST(t_body_in_progress_deadline);
  RUN_TEST(t_write_in_progress_deadline);
  RUN_TEST(t_draining_deadline);
  RUN_TEST(t_none_or_closing_no_deadline);
  RUN_TEST(t_process_tick_expires_ka_idle_closes);
  RUN_TEST(t_process_tick_header_timeout_stages_408_send);
  RUN_TEST(t_process_tick_reinserts_not_yet_expired_deadline);
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_timing_wheel);
  GREATEST_MAIN_END();
}
