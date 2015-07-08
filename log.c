#include <stdarg.h>
#include <libavutil/time.h>
#include "internal.h"

void do_log(const char *mod, const char *fmt, ...)
{
    char logline[512];
    va_list arg_list;

    va_start(arg_list, fmt);
    vsnprintf(logline, sizeof(logline), fmt, arg_list);
    va_end(arg_list);

    printf("[sfxmp %f %s] %s", av_gettime() / 1000000., mod, logline);
}
