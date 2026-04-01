#include <stdint.h>

#include "include/macros.h"
#include "include/req_arena.h"

#if REQ_ARENA_STATS
THREAD_LOCAL uint64_t g_req_arena_new_chunks = 0;
THREAD_LOCAL uint64_t g_req_arena_new_chunk_bytes = 0;
THREAD_LOCAL uint64_t g_req_arena_reset_calls = 0;
THREAD_LOCAL uint64_t g_req_arena_reset_freed_chunks = 0;
THREAD_LOCAL uint64_t g_req_arena_reset_freed_bytes = 0;
THREAD_LOCAL uint64_t g_req_arena_destroy_calls = 0;
THREAD_LOCAL uint64_t g_req_arena_destroy_freed_chunks = 0;
THREAD_LOCAL uint64_t g_req_arena_destroy_freed_bytes = 0;

#if REQ_ARENA_CHUNK_CACHE
THREAD_LOCAL uint64_t g_req_arena_cache_hits = 0;
THREAD_LOCAL uint64_t g_req_arena_cache_misses = 0;
THREAD_LOCAL uint64_t g_req_arena_cache_put_chunks = 0;
THREAD_LOCAL uint64_t g_req_arena_cache_put_bytes = 0;
THREAD_LOCAL uint64_t g_req_arena_cache_drop_chunks = 0;
THREAD_LOCAL uint64_t g_req_arena_cache_drop_bytes = 0;
THREAD_LOCAL uint64_t g_req_arena_cache_count_peak = 0;
THREAD_LOCAL uint64_t g_req_arena_cache_shutdown_freed_chunks = 0;
THREAD_LOCAL uint64_t g_req_arena_cache_shutdown_freed_bytes = 0;
#endif
#endif

#if REQ_ARENA_CHUNK_CACHE
THREAD_LOCAL struct req_arena_chunk *g_req_arena_chunk_cache = NULL;
THREAD_LOCAL uint32_t g_req_arena_chunk_cache_count = 0;
#endif
