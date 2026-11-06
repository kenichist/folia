// bigalloc.dylib: count malloc/calloc/realloc/posix_memalign of >= 32 KB by call stack; print the top stacks at exit.
// DYLD_INSERT_LIBRARIES=bigalloc.dylib BIGALLOC_OUT=<file> <app>
#include <dlfcn.h>
#include <execinfo.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DEPTH 14
#define SLOTS 4096
typedef struct { unsigned long long h; void *pc[DEPTH]; int n; long count; double bytes; } Site;
static Site sites[SLOTS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int busy;
static long total; static double totalBytes;
static void note(size_t sz) {
  if (sz < 32768 || busy) return;
  busy = 1;
  void *pc[DEPTH + 2]; int n = backtrace(pc, DEPTH + 2) - 2; if (n < 0) n = 0;
  unsigned long long h = 1469598103934665603ULL;
  for (int i = 0; i < n; i++) h = (h ^ (unsigned long long)pc[i + 2]) * 1099511628211ULL;
  pthread_mutex_lock(&mu);
  total++; totalBytes += sz;
  for (unsigned k = 0; k < SLOTS; k++) {
    Site *s = &sites[(h + k) % SLOTS];
    if (s->count == 0) { s->h = h; s->n = n; memcpy(s->pc, pc + 2, n * sizeof(void *)); }
    if (s->h == h) { s->count++; s->bytes += sz; break; }
  }
  pthread_mutex_unlock(&mu);
  busy = 0;
}
static void *my_malloc(size_t s) { note(s); return malloc(s); }
static void *my_calloc(size_t a, size_t b) { note(a * b); return calloc(a, b); }
static void *my_realloc(void *p, size_t s) { note(s); return realloc(p, s); }
static int my_pm(void **p, size_t a, size_t s) { note(s); return posix_memalign(p, a, s); }
static void *my_zm(malloc_zone_t *z, size_t s) { note(s); return malloc_zone_malloc(z, s); }
static void *my_zc(malloc_zone_t *z, size_t a, size_t b) { note(a * b); return malloc_zone_calloc(z, a, b); }
static void *my_zr(malloc_zone_t *z, void *p, size_t s) { note(s); return malloc_zone_realloc(z, p, s); }
static void *my_za(malloc_zone_t *z, size_t a, size_t s) { note(s); return malloc_zone_memalign(z, a, s); }
__attribute__((used)) static struct { const void *r, *o; } interposers[] __attribute__((section("__DATA,__interpose"))) = {
  {(const void *)my_malloc, (const void *)malloc}, {(const void *)my_calloc, (const void *)calloc},
  {(const void *)my_realloc, (const void *)realloc}, {(const void *)my_pm, (const void *)posix_memalign},
  {(const void *)my_zm, (const void *)malloc_zone_malloc}, {(const void *)my_zc, (const void *)malloc_zone_calloc},
  {(const void *)my_zr, (const void *)malloc_zone_realloc}, {(const void *)my_za, (const void *)malloc_zone_memalign}};
static int cmp(const void *a, const void *b) { long x = ((const Site *)a)->count, y = ((const Site *)b)->count; return x < y ? 1 : x > y ? -1 : 0; }
__attribute__((destructor)) static void dump(void) {
  busy = 1;
  const char *path = getenv("BIGALLOC_OUT");
  FILE *f = path ? fopen(path, "w") : stderr;
  if (!f) return;
  qsort(sites, SLOTS, sizeof(Site), cmp);
  fprintf(f, "large allocations (>= 32 KB): %ld, %.1f MB\n", total, totalBytes / 1e6);
  for (int i = 0; i < 25 && sites[i].count; i++) {
    fprintf(f, "\n#%d count %ld, %.1f MB, avg %.0f KB\n", i, sites[i].count, sites[i].bytes / 1e6, sites[i].bytes / sites[i].count / 1024);
    char **sym = backtrace_symbols(sites[i].pc, sites[i].n);
    for (int j = 0; j < sites[i].n; j++) fprintf(f, "   %s\n", sym ? sym[j] : "?");
  }
  if (f != stderr) fclose(f);
}
