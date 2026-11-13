#define _GNU_SOURCE
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <pthread.h>

/* LD_PRELOAD malloc wrapper: tracks live bytes and high-water mark. */

static void *(*real_malloc)(size_t) = NULL;
static void (*real_free)(void *) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;

static size_t cur_live = 0;
static size_t peak_live = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static size_t block_size(void *p)
{
  if (!p) return 0;
  return malloc_usable_size(p);
}

static void init(void)
{
  if (!real_malloc) {
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
  }
}

void *malloc(size_t sz)
{
  init();
  void *p = real_malloc(sz);
  pthread_mutex_lock(&lock);
  cur_live += block_size(p);
  if (cur_live > peak_live) peak_live = cur_live;
  if (getenv("MALLOC_TRACE") && sz >= 131072) {
    static int big_count = 0;
    if (big_count < 40) {
      fprintf(stderr, "BIG malloc %zuKB live=%zuKB\n", sz / 1024, cur_live / 1024);
      big_count++;
    }
  }
  pthread_mutex_unlock(&lock);
  return p;
}

void *calloc(size_t n, size_t sz)
{
  init();
  void *p = real_calloc(n, sz);
  pthread_mutex_lock(&lock);
  cur_live += block_size(p);
  if (cur_live > peak_live) peak_live = cur_live;
  pthread_mutex_unlock(&lock);
  return p;
}

void *realloc(void *old, size_t sz)
{
  init();
  size_t old_sz = block_size(old);
  void *p = real_realloc(old, sz);
  pthread_mutex_lock(&lock);
  cur_live = cur_live - old_sz + block_size(p);
  if (cur_live > peak_live) peak_live = cur_live;
  pthread_mutex_unlock(&lock);
  return p;
}

void free(void *p)
{
  if (!p) return;
  init();
  pthread_mutex_lock(&lock);
  cur_live -= block_size(p);
  pthread_mutex_unlock(&lock);
  real_free(p);
}

__attribute__((destructor))
static void report(void)
{
  char *env = getenv("MALLOC_PEAK_REPORT");
  if (env && env[0] == '1')
    fprintf(stderr, "MALLOC_PEAK_KB=%zu LIVE_KB=%zu\n",
            peak_live / 1024, cur_live / 1024);
}
