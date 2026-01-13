#include "logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static log_level_t g_level = LOG_INFO;
static FILE* g_log_file = NULL;

int logger_init(log_level_t level) {
    g_level = level;
    g_log_file = fopen("proxy.log", "a");
    if (!g_log_file) g_log_file = NULL;
    return 0;
}

void logger_finalize(void) {
    if (g_log_file && g_log_file != stderr) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

static void vlog_internal(log_level_t lvl, const char* tag, const char* fmt, va_list ap) {
    if (lvl > g_level) return;

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);

    va_list ap2;
    va_copy(ap2, ap);

    fprintf(stderr, "[%02d:%02d:%02d] %s ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");

    if (g_log_file) {
        fprintf(g_log_file, "[%02d:%02d:%02d] %s ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tag);
        vfprintf(g_log_file, fmt, ap2);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
}

void log_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_internal(LOG_ERROR, "[ERR]", fmt, ap);
    va_end(ap);
}

void log_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_internal(LOG_INFO, "[INF]", fmt, ap);
    va_end(ap);
}

void log_debug(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_internal(LOG_DEBUG, "[DBG]", fmt, ap);
    va_end(ap);
}