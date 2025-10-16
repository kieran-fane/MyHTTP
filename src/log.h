#ifndef MYHTTP_LOG_H
#define MYHTTP_LOG_H

#include <stdio.h>    // FILE*
#include <time.h>     // time_t, struct tm
#include <stdarg.h>   // va_list

/* Set the output stream for logs (defaults to stderr if not called). */
void log_init(FILE *stream);

/* Access log (one line per request). All strings may be NULL.
   Example format:
   2025-10-15T18:42:01Z 127.0.0.1 "GET /" 200 123 2 "curl/8.5" */
void log_access(const char *peer_ip_port,
                const char *method,
                const char *target,
                int status,
                long bytes_sent,
                long duration_ms,
                const char *user_agent);

/* Error logging (printf-style) and perror-style helper. */
void log_error(const char *fmt, ...) __attribute__((format(printf,1,2)));
void log_perror(const char *ctx);

/* Format UTC time into ISO-8601 (e.g., 2025-10-15T18:42:01Z). */
void ts_iso8601(char *buf, size_t len, time_t t);

#endif /* MICROHTTPD_LOG_H */

