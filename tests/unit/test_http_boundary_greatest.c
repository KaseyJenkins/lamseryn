#include "../vendor/greatest_color.h"
#include "../vendor/greatest.h"

#include <string.h>

#include "include/http_boundary.h"

TEST t_finds_terminator_in_buffer(void) {
  const char *pfx = "";
  const char *b = "GET / HTTP/1.1\r\nHost: x\r\n\r\nNEXT";
  ssize_t end = http_find_header_terminator_end(pfx, 0, b, strlen(b));
  ASSERT(end > 0);
  // End offset should point just after the first \r\n\r\n.
  const char *term = "\r\n\r\n";
  const char *pos = strstr(b, term);
  ASSERT(pos != NULL);
  ASSERT_EQ((ssize_t)(pos - b + 4), end);
  PASS();
}

TEST t_finds_terminator_across_prefix_boundary(void) {
  // Prefix contains "\r\n\r" and the buffer begins with "\n".
  const char prefix[3] = {'\r', '\n', '\r'};
  const char *b = "\nGET /2 HTTP/1.1\r\nHost: y\r\n\r\n";
  ssize_t end = http_find_header_terminator_end(prefix, 3, b, strlen(b));
  ASSERT(end > 0);
  // Combined stream: [\r\n\r] + [\n...] => terminator ends at combined index 4.
  ASSERT_EQ((ssize_t)4, end);
  PASS();
}

TEST t_finds_terminator_split_2_and_2(void) {
  // Simulate a recv boundary where the previous chunk ended with "\r\n"
  // and the next chunk begins with "\r\n".
  const char prefix[2] = {'\r', '\n'};
  const char *b = "\r\nGET /next HTTP/1.1\r\nHost: z\r\n\r\n";
  ssize_t end = http_find_header_terminator_end(prefix, 2, b, strlen(b));
  ASSERT(end > 0);
  // Combined stream begins with "\r\n\r\n".
  ASSERT_EQ((ssize_t)4, end);
  PASS();
}

TEST t_returns_minus_one_when_not_found(void) {
  const char *b = "GET / HTTP/1.1\r\nHost: x\r\n";
  ssize_t end = http_find_header_terminator_end(NULL, 0, b, strlen(b));
  ASSERT_EQ((ssize_t)-1, end);
  PASS();
}

SUITE(s_http_boundary) {
  RUN_TEST(t_finds_terminator_in_buffer);
  RUN_TEST(t_finds_terminator_across_prefix_boundary);
  RUN_TEST(t_finds_terminator_split_2_and_2);
  RUN_TEST(t_returns_minus_one_when_not_found);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(s_http_boundary);
  GREATEST_MAIN_END();
}
