// The log's lines with the time since the start (rt.h LOG): "[  12.345] [romfs] ...". A message is written in one piece,
// so lines from threads don't mix; a message that doesn't end its line leaves the next one without a time.
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "rt.h"

static atomic_bool g_line_start = true;

void rt_log(const char *fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof msg) n = sizeof msg - 1;  // (cut)
    char out[sizeof msg + 512];
    size_t o = 0;
    double t = SDL_GetTicksNS() / 1e9;
    bool start = atomic_load(&g_line_start);
    for (int i = 0; i < n && o < sizeof out - 16; i++) {
        if (start) {
            int k = snprintf(out + o, sizeof out - o, "[%8.3f] ", t);
            if (k > 0) o += (size_t)k;
            start = false;
        }
        out[o++] = msg[i];
        if (msg[i] == '\n') start = true;
    }
    atomic_store(&g_line_start, start);
    fwrite(out, 1, o, stderr);
}
