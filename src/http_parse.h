#ifndef MYHTTP_HTTP_PARSE_H
#define MYHTTP_HTTP_PARSE_H

#include <stddef.h>
#include <stdbool.h>

/* Supported methods (HEAD removed for now) */
typedef enum {
  MYHTTP_GET    = 0,
  MYHTTP_POST   = 1,
  MYHTTP_PUT    = 2,
  MYHTTP_PATCH  = 3,
  MYHTTP_DELETE = 4,
  MYHTTP_METHOD_UNKNOWN = 255
} myhttp_method;

/* Parsed request (header values point into caller-owned buffer). */
struct myhttp_req {
  myhttp_method method;   /* GET/POST/PUT/PATCH/DELETE */
  char *target;           /* request-target (raw, not decoded) */
  char *version;          /* e.g., "HTTP/1.1" */

  /* Common headers (NULL if absent) */
  char *h_host;
  char *h_connection;
  char *h_user_agent;

  /* Body-related (NULL if absent) */
  char *h_content_type;
  char *h_content_length;
  char *h_expect;           /* e.g., "100-continue" */

  /* Buffer bookkeeping (caller-owned) */
  char  *buf;
  size_t buf_len;
};

/* Reset fields to a clean state (does not free the buffer). */
void myhttp_req_reset(struct myhttp_req *r);

/* Parse request line + headers from buf[0..len).
   Returns: -1 error, 0 need more, >0 bytes consumed (end-of-headers offset). */
int  myhttp_parse_request(char *buf, size_t len, struct myhttp_req *out);

/* Case-insensitive header lookup within CRLF-terminated header block.
   Returns pointer to value (trimmed leading OWS) or NULL. */
char* myhttp_find_header(char *headers_start, const char *key);

/* In-place percent-decoding of a path segment; 0 on success, -1 on bad %xx. */
int  myhttp_percent_decode_inplace(char *s);

/* Helpers for body handling */
long myhttp_content_length(const struct myhttp_req *r); /* -1 if missing/invalid */
bool myhttp_expect_100(const struct myhttp_req *r);     /* true if Expect: 100-continue */

/* Convenience */
int  myhttp_method_from_token(const char *tok);         /* returns myhttp_method enum */

static inline bool myhttp_wants_close(const struct myhttp_req *r) {
  /* Local strncasecmp substitute to avoid non-standard headers if desired */
  const char *h = r->h_connection;
  if (!h) return false;
  for (int i = 0; i < 5 && h[i]; i++) {
    char a = h[i] | 0x20, b = "close"[i];
    if (a != b) return false;
  }
  return true;
}

#endif /* MYHTTP_HTTP_PARSE_H */

