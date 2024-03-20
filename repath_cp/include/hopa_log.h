#include <stdio.h>
#include <time.h>
#include <stdarg.h>

#define COLOR_RESET "\x1b[0m"
#define COLOR_RED "\x1b[31m"
#define COLOR_GREEN "\x1b[32m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_WHITE "\x1b[37m"

#define HOPA_LOG_INFO(...) log_message(COLOR_WHITE, "HOPA_CP_INFO", __FILE__, __LINE__, __VA_ARGS__)
#define HOPA_LOG_WARN(...) log_message(COLOR_YELLOW, "HOPA_CP_WARN", __FILE__, __LINE__, __VA_ARGS__)
#define HOPA_LOG_ERROR(...) log_message(COLOR_RED, "HOPA_CP_ERROR", __FILE__, __LINE__, __VA_ARGS__)
#define HOPA_LOG_TRACE(...) log_message(COLOR_GREEN, "HOPA_CP_TRACE", __FILE__, __LINE__, __VA_ARGS__)

void log_message(const char *color, const char *level, const char *file, int line, const char *format, ...)
{
    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);

    fprintf(stderr, "%s[%s][%s][%s:%d]: ", color, level, buffer, file, line);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "%s\n", COLOR_RESET);
}