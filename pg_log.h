#ifndef PG_LOG_H
#define PG_LOG_H

#include <stdio.h>

/* Log levels */
typedef enum {
    PG_LOG_DEBUG = 0,
    PG_LOG_INFO = 1,
    PG_LOG_WARN = 2,
    PG_LOG_ERROR = 3
} pg_log_level_t;

/* Set global log level (default: PG_LOG_INFO) */
void pg_log_set_level(pg_log_level_t level);

/* Log functions with module name and level */
#define PG_LOG_DEBUG(module, ...) \
    pg_log_impl(PG_LOG_DEBUG, (module), __FILE__, __LINE__, __VA_ARGS__)

#define PG_LOG_INFO(module, ...) \
    pg_log_impl(PG_LOG_INFO, (module), __FILE__, __LINE__, __VA_ARGS__)

#define PG_LOG_WARN(module, ...) \
    pg_log_impl(PG_LOG_WARN, (module), __FILE__, __LINE__, __VA_ARGS__)

#define PG_LOG_ERROR(module, ...) \
    pg_log_impl(PG_LOG_ERROR, (module), __FILE__, __LINE__, __VA_ARGS__)

/* Implementation function (should not be called directly) */
void pg_log_impl(pg_log_level_t level, const char *module, const char *file, 
                 int line, const char *fmt, ...);

#endif /* PG_LOG_H */
