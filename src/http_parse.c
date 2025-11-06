#include "http_parse.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

/* ---------------- Reset ---------------- */
void myhttp_req_reset(struct myhttp_req *r) {
    if (!r) return;
    r->method = MYHTTP_METHOD_UNKNOWN;

    r->target = NULL;
    r->version = NULL;

    r->h_host = NULL;
    r->h_connection = NULL;
    r->h_user_agent = NULL;

    r->h_content_type = NULL;
    r->h_content_length = NULL;
    r->h_expect = NULL;

    /* Caller owns r->buf; keep pointer, just reset length view. */
    r->buf_len = 0;
}

/* ----------- Small helpers ----------- */
static const char* find_crlf(const char *p, const char *end) {
    for (const char *q = p; q + 1 < end; ++q) {
        if (q[0] == '\r' && q[1] == '\n') return q;
    }
    return NULL;
}

/* Trim trailing spaces/tabs by writing a terminating NUL. */
static void rtrim_ows(char *start, char *line_end) {
    while (line_end > start && (line_end[-1] == ' ' || line_end[-1] == '\t')) {
        --line_end;
    }
    *line_end = '\0';
}

/* ---------------- Parser ---------------- */
int myhttp_parse_request(char *buf, size_t len, struct myhttp_req *out) {
    if (!buf || !out) return -1;

    /* Initialize out view. */
    out->buf = buf;
    out->buf_len = len;

    const char *p = buf;
    const char *end = buf + len;

    /* --- Request line --- */
    const char *line_end = find_crlf(p, end);
    if (!line_end) return 0; /* need more data */

    /* METHOD SP TARGET SP VERSION CRLF */
    const char *sp1 = memchr(p, ' ', (size_t)(line_end - p));
    if (!sp1) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
    if (!sp2) return -1;

    /* In-place tokenization */
    char *method_tok = (char *)p;
    char *target_tok = (char *)(sp1 + 1);
    char *version_tok = (char *)(sp2 + 1);

    *(char *)sp1 = '\0';
    *(char *)sp2 = '\0';
    *(char *)line_end = '\0'; /* terminate VERSION at end of line */

    out->method = myhttp_method_from_token(method_tok);
    if (out->method == MYHTTP_METHOD_UNKNOWN) return -1;

    /* MVP: only HTTP/1.1 */
    if (strcmp(version_tok, "HTTP/1.1") != 0) return -1;

    out->target  = target_tok;
    out->version = version_tok;

    /* --- Headers --- */
    printf("HEADERS: \n");
    const char *hp = line_end + 2; /* first header line */
    while (hp < end) {
        /* End of headers? */
        if ((end - hp) >= 2 && hp[0] == '\r' && hp[1] == '\n') {
            hp += 2; /* consume CRLFCRLF */
            return (int)(hp - buf); /* offset to body start */
        }

        const char *hdr_end = find_crlf(hp, end);
        if (!hdr_end) return 0; /* need more data */

        const char *colon = memchr(hp, ':', (size_t)(hdr_end - hp));
        if (!colon) return -1; /* malformed header line */

        size_t key_len = (size_t)(colon - hp);
        char *val = (char *)(colon + 1);

        /* skip leading OWS in value */
        while (val < hdr_end && (*val == ' ' || *val == '\t')) val++;

        /* NUL-terminate value at end-of-line and trim trailing OWS */
        rtrim_ows(val, (char *)hdr_end);
	printf("[%s:%s]\t", hp, val);
        /* Case-insensitive match on header name by exact length */
        if (key_len == 4  && strncasecmp(hp, "Host", 4) == 0) {
            out->h_host = val;
        } else if (key_len == 10 && strncasecmp(hp, "Connection", 10) == 0) {
            out->h_connection = val;
        } else if (key_len == 14 && strncasecmp(hp, "Content-Length", 14) == 0) {
            out->h_content_length = val;
        } else if (key_len == 12 && strncasecmp(hp, "Content-Type", 12) == 0) {
            out->h_content_type = val;
        } else if (key_len == 6  && strncasecmp(hp, "Expect", 6) == 0) {
            out->h_expect = val;
        } else if (key_len == 10 && strncasecmp(hp, "User-Agent", 10) == 0) {
            out->h_user_agent = val;
        }

        hp = hdr_end + 2; /* next header line */
    }

    return 0; /* need more data */
}

/* ---------------- Header lookup (optional helper) ---------------- */
char* myhttp_find_header(char *headers_start, const char *key) {
    if (!headers_start || !key) return NULL;

    size_t key_len = strlen(key);
    char *p = headers_start;

    while (*p) {
        /* End of headers? */
        if (p[0] == '\r' && p[1] == '\n')
            break;

        /* Find end of this header line */
        char *line_end = strstr(p, "\r\n");
        if (!line_end) break;

        /* Find colon */
        char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t name_len = (size_t)(colon - p);

            if (name_len == key_len) {
                size_t i = 0;
                for (; i < key_len; i++) {
                    if (tolower((unsigned char)p[i]) != tolower((unsigned char)key[i]))
                        break;
                }
                if (i == key_len) {
                    char *val = colon + 1;
                    while (*val == ' ' || *val == '\t') val++;
                    return val; /* points into buffer, NUL before CRLF if parser trimmed it */
                }
            }
        }
        p = line_end + 2;
    }
    return NULL;
}

/* ---------------- Percent decode ---------------- */
int myhttp_percent_decode_inplace(char *s) {
    if (!s) return -1;

    char *src = s;
    char *dst = s;

    while (*src) {
        if (*src == '%') {
            if (!isxdigit((unsigned char)src[1]) ||
                !isxdigit((unsigned char)src[2])) {
                return -1; /* malformed */
            }
            unsigned int hi = (unsigned int)(isdigit((unsigned char)src[1])
                                ? src[1] - '0'
                                : (tolower((unsigned char)src[1]) - 'a' + 10));
            unsigned int lo = (unsigned int)(isdigit((unsigned char)src[2])
                                ? src[2] - '0'
                                : (tolower((unsigned char)src[2]) - 'a' + 10));
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return 0;
}

/* ---------------- Content-Length / Expect helpers ---------------- */
long myhttp_content_length(const struct myhttp_req *r) {
    if (!r || !r->h_content_length) return -1;

    errno = 0;
    char *endp = NULL;
    long v = strtol(r->h_content_length, &endp, 10);

    if (errno == ERANGE || v < 0) return -1;
    /* Accept if endp is NUL (parser trimmed) or points to CR/LF/OWS. */
    if (endp == r->h_content_length) return -1;
    while (*endp == ' ' || *endp == '\t') endp++;
    if (*endp != '\0') return -1;

    return v;
}

bool myhttp_expect_100(const struct myhttp_req *r) {
    if (!r || !r->h_expect) return false;
    const char *v = r->h_expect;
    while (*v == ' ' || *v == '\t') v++;
    /* Case-insensitive match for exactly "100-continue" */
    const char *t = "100-continue";
    for (size_t i = 0; t[i]; i++) {
        if (tolower((unsigned char)v[i]) != (unsigned char)t[i]) return false;
    }
    char next = v[strlen(t)];
    return (next == '\0'); /* we trimmed line end to NUL in the parser */
}

/* ---------------- Method mapping ---------------- */
int myhttp_method_from_token(const char *tok) {
    if (!tok) return MYHTTP_METHOD_UNKNOWN;
    if (strcmp(tok, "GET") == 0)    return MYHTTP_GET;
    if (strcmp(tok, "POST") == 0)   return MYHTTP_POST;
    if (strcmp(tok, "PUT") == 0)    return MYHTTP_PUT;
    if (strcmp(tok, "PATCH") == 0)  return MYHTTP_PATCH;
    if (strcmp(tok, "DELETE") == 0) return MYHTTP_DELETE;
    return MYHTTP_METHOD_UNKNOWN;
}

