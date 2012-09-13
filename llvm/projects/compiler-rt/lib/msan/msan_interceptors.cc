#include "msan_interface.h"
#include "msan.h"
#include "sanitizer_common/sanitizer_common.h"
#include <interception/interception.h>

#include <stdarg.h>
// ACHTUNG! No other system header includes in this file.
// Ideally, we should get rid of stdarg.h as well.

typedef uptr size_t;
typedef sptr ssize_t;
typedef u64  off_t;
using namespace __msan;

#define ENSURE_MSAN_INITED() do { \
    CHECK(!msan_init_is_running);       \
  if (!msan_inited) { \
    __msan_init(); \
  } \
} while (0)

#define CHECK_UNPOISONED(x, n) \
  do { \
  sptr offset = __msan_test_shadow(x, n); \
  if (offset >= 0) { \
  Printf("UMR in %s at offset %d\n", __FUNCTION__, offset); \
  __msan_warning(); \
  } \
  } while (0)

static void *fast_memset(void *ptr, int c, size_t n);
static void *fast_memcpy(void *dst, const void *src, size_t n);

INTERCEPTOR(size_t, fread, void *ptr, size_t size, size_t nmemb, void *file) {
  ENSURE_MSAN_INITED();
  size_t res = REAL(fread)(ptr, size, nmemb, file);
  if (res > 0)
    __msan_unpoison(ptr, res * size);
  return res;
}

INTERCEPTOR(ssize_t, read, int fd, void *ptr, size_t count) {
  ENSURE_MSAN_INITED();
  ssize_t res = REAL(read)(fd, ptr, count);
  if (res > 0)
    __msan_unpoison(ptr, res);
  return res;
}

INTERCEPTOR(ssize_t, pread, int fd, void *ptr, size_t count, off_t offset) {
  ENSURE_MSAN_INITED();
  ssize_t res = REAL(pread)(fd, ptr, count, offset);
  if (res > 0)
    __msan_unpoison(ptr, res);
  return res;
}

INTERCEPTOR(void*, memcpy, void* dest, const void* src, size_t n) {
  ENSURE_MSAN_INITED();
  void* res = fast_memcpy(dest, src, n);
  __msan_copy_poison(dest, src, n);
  return res;
}

INTERCEPTOR(void*, memmove, void* dest, const void* src, size_t n) {
  ENSURE_MSAN_INITED();
  void* res = REAL(memmove)(dest, src, n);
  __msan_move_poison(dest, src, n);
  return res;
}

INTERCEPTOR(void*, memset, void *s, int c, size_t n) {
  ENSURE_MSAN_INITED();
  void* res = fast_memset(s, c, n);
  if (MEM_TO_SHADOW((uptr)s) != (uptr)s)
    __msan_unpoison(s, n);
  return res;
}

INTERCEPTOR(int, posix_memalign, void **memptr, size_t alignment, size_t size) {
  GET_MALLOC_STACK_TRACE;
  CHECK_EQ(alignment & (alignment - 1), 0);
  *memptr = MsanReallocate(&stack, 0, size, alignment, false);
  CHECK_NE(memptr, 0);
  return 0;
}

INTERCEPTOR(void, free, void *ptr) {
  ENSURE_MSAN_INITED();
  if (ptr == 0) return;
  MsanDeallocate(ptr);
}

INTERCEPTOR(size_t, strlen, const char* s) {
  ENSURE_MSAN_INITED();
  size_t res = REAL(strlen)(s);
  CHECK_UNPOISONED(s, res + 1);
  return res;
}

INTERCEPTOR(size_t, strnlen, const char* s, size_t n) {
  ENSURE_MSAN_INITED();
  size_t res = REAL(strnlen)(s, n);
  size_t scan_size = (res == n) ? res : res + 1;
  CHECK_UNPOISONED(s, scan_size);
  return res;
}

INTERCEPTOR(char*, strcpy, char* dest, const char* src) {
  ENSURE_MSAN_INITED();
  size_t n = REAL(strlen)(src);
  char* res = REAL(strcpy)(dest, src);
  __msan_copy_poison(dest, src, n + 1);
  return res;
}

INTERCEPTOR(char*, strncpy, char* dest, const char* src, size_t n) {
  ENSURE_MSAN_INITED();
  size_t copy_size = REAL(strnlen)(src, n);
  if (copy_size < n)
    copy_size++; // trailing \0
  char* res = REAL(strncpy)(dest, src, n);
  __msan_copy_poison(dest, src, copy_size);
  return res;
}

INTERCEPTOR(char*, gcvt, double number, size_t ndigit, char* buf) {
  ENSURE_MSAN_INITED();
  char* res = REAL(gcvt)(number, ndigit, buf);
  if (!__msan_has_dynamic_component()) {
    size_t n = REAL(strlen)(buf);
    __msan_unpoison(buf, n + 1);
  }
  return res;
}

INTERCEPTOR(char*, strcat, char* dest, const char* src) {
  ENSURE_MSAN_INITED();
  size_t src_size = REAL(strlen)(src);
  size_t dest_size = REAL(strlen)(dest);
  char* res = REAL(strcat)(dest, src);
  __msan_copy_poison(dest + dest_size, src, src_size + 1);
  return res;
}

INTERCEPTOR(char*, strncat, char* dest, const char* src, size_t n) {
  ENSURE_MSAN_INITED();
  size_t dest_size = REAL(strlen)(dest);
  size_t copy_size = REAL(strlen)(src);
  if (copy_size < n)
    copy_size++; // trailing \0
  char* res = REAL(strncat)(dest, src, n);
  __msan_copy_poison(dest + dest_size, src, copy_size);
  return res;
}

INTERCEPTOR(long, strtol, const char *nptr, char **endptr, int base) {
  long res = REAL(strtol)(nptr, endptr, base);
  if (!__msan_has_dynamic_component()) {
    __msan_unpoison(endptr, sizeof(*endptr));
  }
  return res;
}

INTERCEPTOR(long long , strtoll, const char *nptr, char **endptr, int base) {
  long res = REAL(strtoll)(nptr, endptr, base);
  if (!__msan_has_dynamic_component()) {
    __msan_unpoison(endptr, sizeof(*endptr));
  }
  return res;
}

INTERCEPTOR(int, vsnprintf, char *str, uptr size,
            const char *format, va_list ap) {
  int res = REAL(vsnprintf)(str, size, format, ap);
  if (!__msan_has_dynamic_component()) {
    __msan_unpoison(str, res + 1);
  }
  return res;
}

INTERCEPTOR(int, vsprintf, char *str, const char *format, va_list ap) {
  int res = REAL(vsprintf)(str, format, ap);
  if (!__msan_has_dynamic_component()) {
    __msan_unpoison(str, res + 1);
  }
  return res;
}

INTERCEPTOR(int, vswprintf, void *str, uptr size, void *format, va_list ap) {
  int res = REAL(vswprintf)(str, size, format, ap);
  if (!__msan_has_dynamic_component()) {
    __msan_unpoison(str, 4 * (res + 1));
  }
  return res;
}

INTERCEPTOR(int, sprintf, char *str, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int res = vsprintf(str, format, ap);
  va_end(ap);
  return res;
}

INTERCEPTOR(int, snprintf, char *str, uptr size, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int res = vsnprintf(str, size, format, ap);
  va_end(ap);
  return res;
}

INTERCEPTOR(int, swprintf, void *str, uptr size, void *format, ...) {
  va_list ap;
  va_start(ap, format);
  int res = vswprintf(str, size, format, ap);
  va_end(ap);
  return res;
}

INTERCEPTOR(int, gettimeofday, void *tv, void *tz) {
  int res = REAL(gettimeofday)(tv, tz);
  if (tv)
    __msan_unpoison(tv, 16);
  if (tz)
    __msan_unpoison(tz, 8);
  return res;
}

INTERCEPTOR(char *, fcvt, double x, int a, int *b, int *c) {
  char *res = REAL(fcvt)(x, a, b, c);
  if (!__msan_has_dynamic_component()) {
    __msan_unpoison(b, sizeof(*b));
    __msan_unpoison(c, sizeof(*c));
  }
  return res;
}

INTERCEPTOR(char*, getenv, char* name) {
  ENSURE_MSAN_INITED();
  char* res = REAL(getenv)(name);
  if (!__msan_has_dynamic_component()) {
    if (res)
      __msan_unpoison(res, REAL(strlen)(res) + 1);
  }
  return res;
}

INTERCEPTOR(int, __fxstat, int magic, int fd, void* buf) {
  ENSURE_MSAN_INITED();
  int res = REAL(__fxstat)(magic, fd, buf);
  if (!res)
    __msan_unpoison(buf, 144); // seems like a reasonable size ;)
  return res;
}

INTERCEPTOR(int, __xstat, int magic, char* path, void* buf) {
  ENSURE_MSAN_INITED();
  int res = REAL(__xstat)(magic, path, buf);
  if (!res)
    __msan_unpoison(buf, 144);
  return res;
}

INTERCEPTOR(int, __lxstat, int magic, char* path, void* buf) {
  ENSURE_MSAN_INITED();
  int res = REAL(__lxstat)(magic, path, buf);
  if (!res)
    __msan_unpoison(buf, 144);
  return res;
}

INTERCEPTOR(int, pipe, int pipefd[2]) {
  ENSURE_MSAN_INITED();
  int res = REAL(pipe)(pipefd);
  if (!res)
    __msan_unpoison(pipefd, sizeof(int[2]));
  return res;
}

INTERCEPTOR(int, wait, int* status) {
  ENSURE_MSAN_INITED();
  int res = REAL(wait)(status);
  if (status)
    __msan_unpoison(status, sizeof(*status));
  return res;
}

INTERCEPTOR(int, waitpid, int pid, int* status, int options) {
  ENSURE_MSAN_INITED();
  int res = REAL(waitpid)(pid, status, options);
  if (status)
    __msan_unpoison(status, sizeof(*status));
  return res;
}

INTERCEPTOR(char*, fgets, char* s, int size, void* stream) {
  ENSURE_MSAN_INITED();
  char* res = REAL(fgets)(s, size, stream);
  if (res)
    __msan_unpoison(s, REAL(strlen)(s) + 1);
  return res;
}

INTERCEPTOR(char*, getcwd, char* buf, size_t size) {
  ENSURE_MSAN_INITED();
  char* res = REAL(getcwd)(buf, size);
  if (res)
    __msan_unpoison(buf, REAL(strlen)(buf) + 1);
  return res;
}

INTERCEPTOR(char*, realpath, char* path, char* abspath) {
  ENSURE_MSAN_INITED();
  char* res = REAL(realpath)(path, abspath);
  if (res)
    __msan_unpoison(abspath, REAL(strlen)(abspath) + 1);
  return res;
}

INTERCEPTOR(void *, calloc, size_t nmemb, size_t size) {
  GET_MALLOC_STACK_TRACE;
  if (!msan_inited) {
    // Hack: dlsym calls calloc before REAL(calloc) is retrieved from dlsym.
    const size_t kCallocPoolSize = 1024;
    static uptr calloc_memory_for_dlsym[kCallocPoolSize];
    static size_t allocated;
    size_t size_in_words = ((nmemb * size) + kWordSize - 1) / kWordSize;
    void *mem = (void*)&calloc_memory_for_dlsym[allocated];
    allocated += size_in_words;
    CHECK(allocated < kCallocPoolSize);
    return mem;
  }

  return MsanReallocate(&stack, 0, nmemb * size, sizeof(u64), true);
}

INTERCEPTOR(void *, realloc, void *ptr, size_t size) {
  GET_MALLOC_STACK_TRACE;
  return MsanReallocate(&stack, ptr, size, sizeof(u64), false);
}

INTERCEPTOR(void *, malloc, size_t size) {
  GET_MALLOC_STACK_TRACE;
  return MsanReallocate(&stack, 0, size, sizeof(u64), false);
}

// static
void *fast_memset(void *ptr, int c, size_t n) {
#if 1
  // hack until we have a really fast internal_memset
  if (sizeof(uptr) == 8 &&
      (n % 8) == 0 &&
      ((uptr)ptr % 8) == 0 &&
      (c == 0 || c == -1)) {
    // Printf("memset %p %zd %x\n", ptr, n, c);
    uptr to_store = c ? -1L : 0L;
    uptr *p = (uptr*)ptr;
    for (size_t i = 0; i < n / 8; i++)
      p[i] = to_store;
    return ptr;
  }
#endif
  CHECK(REAL(memset));
  return REAL(memset)(ptr, c, n);
}

// static
void *fast_memcpy(void *dst, const void *src, size_t n) {
#if 1
  // Same hack as in fast_memset above.
  if (sizeof(uptr) == 8 &&
      (n % 8) == 0 &&
      ((uptr)dst % 8) == 0 &&
      ((uptr)src % 8) == 0) {
    uptr *d = (uptr*)dst;
    uptr *s = (uptr*)src;
    for (size_t i = 0; i < n / 8; i++)
      d[i] = s[i];
    return dst;
  }
#endif
  CHECK(REAL(memcpy));
  return REAL(memcpy)(dst, src, n);
}

#define IS_IN_SHADOW(x) (MEM_TO_SHADOW(((uptr)x)) == (uptr)x)

// These interface functions reside here so that they can use
// fast_memset, etc.

void __msan_unpoison(void *a, uptr size) {
  if (IS_IN_SHADOW(a)) return;
  fast_memset((void*)MEM_TO_SHADOW((uptr)a), 0, size);
}

void __msan_poison(void *a, uptr size) {
  if (IS_IN_SHADOW(a)) return;
  fast_memset((void*)MEM_TO_SHADOW((uptr)a),
                  __msan::flags.poison_heap_with_zeroes ? 0 : -1, size);
}

void __msan_poison_stack(void *a, uptr size) {
  if (IS_IN_SHADOW(a)) return;
  fast_memset((void*)MEM_TO_SHADOW((uptr)a),
                  __msan::flags.poison_stack_with_zeroes ? 0 : -1, size);
}

void __msan_clear_and_unpoison(void *a, uptr size) {
  fast_memset(a, 0, size);
  fast_memset((void*)MEM_TO_SHADOW((uptr)a), 0, size);
}

void __msan_copy_origin(void *dst, const void *src, uptr size) {
  if (!__msan_track_origins) return;
  if (!MEM_IS_APP(dst) || !MEM_IS_APP(src)) return;
  uptr d = MEM_TO_ORIGIN(dst);
  uptr s = MEM_TO_ORIGIN(src);
  // FIXME: this is slow and not precise for unaligned data.
  // FIXME: handle memmove case.
  for (uptr i = 0; i < size; i++)
    *(char*)(d+i) = *(char*)(s+i);
}

void __msan_copy_poison(void *dst, const void *src, uptr size) {
  if (IS_IN_SHADOW(dst)) return;
  if (IS_IN_SHADOW(src)) return;
  fast_memcpy((void*)MEM_TO_SHADOW((uptr)dst),
              (void*)MEM_TO_SHADOW((uptr)src), size);
  __msan_copy_origin(dst, src, size);
}

void __msan_move_poison(void *dst, const void *src, uptr size) {
  if (IS_IN_SHADOW(dst)) return;
  if (IS_IN_SHADOW(src)) return;
  CHECK(REAL(memmove));
  REAL(memmove)((void*)MEM_TO_SHADOW((uptr)dst),
         (void*)MEM_TO_SHADOW((uptr)src), size);
  __msan_copy_origin(dst, src, size);
}

void __msan_memcpy_with_poison(void *dst, const void *src, uptr size) {
  memcpy(dst, src, size);  // Calls our interceptor.
}

#undef IS_IN_SHADOW

namespace __msan {
void InitializeInterceptors() {
  static int inited = 0;
  CHECK_EQ(inited, 0);
  inited = 1;
  CHECK(INTERCEPT_FUNCTION(posix_memalign));
  CHECK(INTERCEPT_FUNCTION(malloc));
  CHECK(INTERCEPT_FUNCTION(calloc));
  CHECK(INTERCEPT_FUNCTION(realloc));
  CHECK(INTERCEPT_FUNCTION(free));
  CHECK(INTERCEPT_FUNCTION(fread));
  CHECK(INTERCEPT_FUNCTION(read));
  CHECK(INTERCEPT_FUNCTION(pread));
  CHECK(INTERCEPT_FUNCTION(memcpy));
  CHECK(INTERCEPT_FUNCTION(memset));
  CHECK(INTERCEPT_FUNCTION(memmove));
  CHECK(INTERCEPT_FUNCTION(strcpy));
  CHECK(INTERCEPT_FUNCTION(strncpy));
  CHECK(INTERCEPT_FUNCTION(strlen));
  CHECK(INTERCEPT_FUNCTION(strnlen));
  CHECK(INTERCEPT_FUNCTION(gcvt));
  CHECK(INTERCEPT_FUNCTION(strcat));
  CHECK(INTERCEPT_FUNCTION(strncat));
  CHECK(INTERCEPT_FUNCTION(strtol));
  CHECK(INTERCEPT_FUNCTION(strtoll));
  CHECK(INTERCEPT_FUNCTION(vsprintf));
  CHECK(INTERCEPT_FUNCTION(vsnprintf));
  CHECK(INTERCEPT_FUNCTION(vswprintf));
  CHECK(INTERCEPT_FUNCTION(sprintf));
  CHECK(INTERCEPT_FUNCTION(snprintf));
  CHECK(INTERCEPT_FUNCTION(swprintf));
  CHECK(INTERCEPT_FUNCTION(getenv));
  CHECK(INTERCEPT_FUNCTION(gettimeofday));
  CHECK(INTERCEPT_FUNCTION(fcvt));
  CHECK(INTERCEPT_FUNCTION(__fxstat));
  CHECK(INTERCEPT_FUNCTION(__xstat));
  CHECK(INTERCEPT_FUNCTION(__lxstat));
  CHECK(INTERCEPT_FUNCTION(pipe));
  CHECK(INTERCEPT_FUNCTION(wait));
  CHECK(INTERCEPT_FUNCTION(waitpid));
  CHECK(INTERCEPT_FUNCTION(fgets));
  CHECK(INTERCEPT_FUNCTION(getcwd));
  CHECK(INTERCEPT_FUNCTION(realpath));
}
}  // namespace __msan
