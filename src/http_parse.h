#ifndef MYHTTP_HTTP_PARSE_H
#define MYHTTP_HTTP_PARSE_H

#include <stddef.h>   // size_t
#include <stdbool.h>

/* Supported methods */
typedef enum { MH_GET = 0, MH_HEAD = 1, MH_METHOD_UNKNOWN = 255 } mh_method;

/* Parsed HTTP request; header pointers point into the supplied buffer. */
struct mh_req {
    mh_method method;     /* MH_GET, MH_HEAD, or MH_METHOD_UNKNOWN */
    char *target;         /* request-target (raw, not decoded) */
    char *version;        /* e.g., "HTTP/1.1" */

    /* Common headers (optional; NULL if absent) */
    char *h_host;
    char *h_connection;
    char *h_user_agent;

    /* Buffer bookkeeping (caller-owned buffer) */
    char  *buf;           /* points to caller's read buffer */
    size_t buf_len;       /* bytes currently in buf */
};

/* Reset fields to a clean state (doesn't touch the underlying buffer). */
void http_req_reset(struct mh_req *r);

/* Parse an HTTP/1.1 request from 'buf[0..len)'. 
   On success: returns >0 (bytes consumed) and fills 'out'.
   If incomplete: returns 0 (need more bytes).
   On error: returns -1 (malformed/over limit). */
int  http_parse_request(char *buf, size_t len, struct mh_req *out);

/* Case-insensitive header lookup within a CRLF-terminated header block.
   Returns pointer to header value start (trimmed of leading OWS) or NULL. */
char* http_find_header(char *headers_start, const char *key);

/* In-place percent-decoding of a path segment (safe set).
   Returns 0 on success, -1 on invalid %xx. */
int  http_percent_decode_inplace(char *s);

/* Helper: does the request explicitly ask to close the connection? */
bool http_wants_close(const struct mh_req *r);

#endif /* MICROHTTPD_HTTP_PARSE_H */

