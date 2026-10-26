/* Linux-only fault injection for persistent process fixtures, never linked into
 * fsnet. Build with cc -shared -fPIC slow_fsync.c -o slow_fsync.so -ldl -pthread
 * and opt in through LD_PRELOAD. Delays preserve the real fsync and durability. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int (*real_fsync)(int);
static struct timespec started;
static pthread_once_t initialized = PTHREAD_ONCE_INIT;

static void initialize(void) {
    real_fsync = dlsym(RTLD_NEXT, "fsync");
    clock_gettime(CLOCK_MONOTONIC, &started);
}

int fsync(int descriptor) {
    pthread_once(&initialized, initialize);
    const char *setting = getenv("FSNET_TEST_FSYNC_DELAY_MS");
    if (setting) {
        long milliseconds = strtol(setting, 0, 10);
        const char *warmup = getenv("FSNET_TEST_FSYNC_SLOW_AFTER_MS");
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000;
        if (milliseconds > 0 && milliseconds <= 500 && elapsed >= (warmup ? strtol(warmup, 0, 10) : 0)) {
            struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
            nanosleep(&delay, 0);
        }
    }
    return real_fsync(descriptor);
}
