#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

#include "pg_log.h"

static pg_log_level_t g_log_level = PG_LOG_INFO;

void pg_log_set_level(pg_log_level_t level)
{
    g_log_level = level;
}

static const char *level_to_string(pg_log_level_t level)
{
    switch (level) {
        case PG_LOG_DEBUG: return "DEBUG";
        case PG_LOG_INFO:  return "INFO";
        case PG_LOG_WARN:  return "WARN";
        case PG_LOG_ERROR: return "ERROR";
        default:           return "UNKNOWN";
    }
}

void pg_log_impl(pg_log_level_t level, const char *module, const char *file,
                 int line, const char *fmt, ...)
{
    va_list args;
    const char *filename;
    
    /* Filter by log level */
    if (level < g_log_level) {
        return;
    }

    /* Extract just the filename from the full path */
    filename = strrchr(file, '/');
    if (!filename) {
        filename = strrchr(file, '\\');
    }
    if (filename) {
        filename++;
    } else {
        filename = file;
    }

    /* Print log header to stderr */
    fprintf(stderr, "[%s] %s:%d (%s) ",
            level_to_string(level),
            filename,
            line,
            module);

    /* Print the actual message */
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}
