/* Linux/glibc diagnostic preload, never linked into PACE or timed comparisons.
 * Counts allocation API calls on the single frontend reactor's calling thread.
 * SIGTERM writes counters using only integer formatting + write, then exits.
 * Startup, warmup and measurement are included; other threads are excluded. */
#include <signal.h>
#include <stddef.h>
#include <unistd.h>
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
static __thread volatile sig_atomic_t calls[3] __attribute__((tls_model("initial-exec")));
void *malloc(size_t n) { ++calls[0]; return __libc_malloc(n); }
void *calloc(size_t n, size_t s) { ++calls[1]; return __libc_calloc(n, s); }
void *realloc(void *p, size_t n) { ++calls[2]; return __libc_realloc(p, n); }
static void finish(int signal_number) {
    (void)signal_number;
    char out[96]; size_t pos = 0;
    const char *keys[] = {"{\"malloc\":", ",\"calloc\":", ",\"realloc\":"};
    for (int i = 0; i < 3; ++i) {
        for (const char *s = keys[i]; *s; ++s) out[pos++] = *s;
        unsigned value = (unsigned)calls[i]; char digits[16]; size_t count = 0;
        do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
        while (count) out[pos++] = digits[--count];
    }
    out[pos++] = '}'; out[pos++] = '\n';
    const ssize_t written = write(STDERR_FILENO, out, pos);
    (void)written;
    _exit(0);
}
__attribute__((constructor)) static void setup(void) { signal(SIGTERM, finish); }
