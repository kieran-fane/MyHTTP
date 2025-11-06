#define _POSIX_C_SOURCE 200809L

#include "http_parse.h"
#include "workq.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <strings.h>

#include <unistd.h>           // close, write, access
#include <signal.h>           // signal, SIGPIPE
#include <arpa/inet.h>        // inet_ntop, htons, htonl
#include <netinet/in.h>       // sockaddr_in, sockaddr_in6
#include <sys/socket.h>       // socket, bind, listen, accept, recv, send
#include <sys/stat.h>         // stat, fstat
#include <limits.h>           // PATH_MAX
#include <pthread.h>          // pthreads

#ifndef BACKLOG
#define BACKLOG 128
#endif

#ifndef RECV_BUF_SZ
#define RECV_BUF_SZ (64 * 1024)
#endif

#ifndef N_WORKERS
#define N_WORKERS 8
#endif

struct config {
	int         port;
	const char *dir;
};

static char g_docroot[PATH_MAX];   /* realpath() of the configured root */

static void usage(const char *prog) {
	fprintf(stderr, "Usage: %s [-p port] [-d root]\n", prog);
	fprintf(stderr, "Defaults: port=8080, root='.'\n");
}

static int parse_int(const char *s) {
	char *end = NULL;
	long v = strtol(s, &end, 10);
	if (!s[0] || (end && *end != '\0') || v < 1 || v > 65535)
		return -1;
	return (int)v;
}

static const char *peer_to_str(const struct sockaddr_storage *ss, char *out, size_t outlen, int *port_out) {
	void *addr = NULL;
	int   port = 0;
	if (ss->ss_family == AF_INET) {
		const struct sockaddr_in *in = (const struct sockaddr_in*)ss;
		addr = (void*)&in->sin_addr;
		port = ntohs(in->sin_port);
		inet_ntop(AF_INET, addr, out, (socklen_t)outlen);
	} else if (ss->ss_family == AF_INET6) {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6*)ss;
		addr = (void*)&in6->sin6_addr;
		port = ntohs(in6->sin6_port);
		inet_ntop(AF_INET6, addr, out, (socklen_t)outlen);
    	} else {
		snprintf(out, outlen, "unknown");
		port = 0;
	}
	if (port_out) *port_out = port;
	return out;
}

static int send_simple_response(int fd, int code, const char *reason, const char *body) {
	char hdr[256];
	size_t blen = body ? strlen(body) : 0;
	int n = snprintf(hdr, sizeof(hdr),
        	"HTTP/1.1 %d %s\r\n"
		"Content-Length: %zu\r\n"
		"Content-Type: text/plain; charset=utf-8\r\n"
		"Connection: keep-alive\r\n"
		"\r\n",
		code, reason, blen);
	if (n < 0 || (size_t)n >= sizeof(hdr)) return -1;

	if (send(fd, hdr, (size_t)n, 0) < 0) return -1;
	if (blen && send(fd, body, blen, 0) < 0) return -1;
	return 0;
}

/* Split request-target into a path (no query/fragment), then percent-decode in place. */
static int extract_decoded_path(const struct myhttp_req *req, char *out, size_t outlen) {
	if (!req->target) { errno = EINVAL; return -1; }
	size_t n = 0;
	const char *t = req->target;
	while (*t && *t != '?' && *t != '#') {
		if (n + 1 >= outlen) { errno = ENAMETOOLONG; return -1; }
		out[n++] = *t++;
	}
	out[n] = '\0';
	if (myhttp_percent_decode_inplace(out) < 0) { errno = EINVAL; return -1; }
	if (out[0] == '\0') { out[0] = '/'; out[1] = '\0'; } // normalize empty -> "/"
    	return 0;
}
#if 0
/* Discard exactly 'need' body bytes, using what's already in buf after 'consumed'.
   Adjusts *used and compacts any extra (pipelined) bytes back to the front. */
static int discard_body_and_compact(int fd, char *buf, size_t *used,
                                    size_t consumed, size_t need) {
	size_t after = *used - consumed;
	if (after >= need) {
		size_t remain = after - need;
		memmove(buf, buf + consumed + need, remain);
		*used = remain;
		return 0;
	}
	*used = 0;
	size_t left = need - after;
	char scratch[16 * 1024];
	while (left > 0) {
		size_t chunk = left < sizeof(scratch) ? left : sizeof(scratch);
		ssize_t n = recv(fd, scratch, chunk, 0);
	if (n <= 0) return -1;
		left -= (size_t)n;
	}
	return 0;
}
#endif
/* Keep-alive decision:
   - HTTP/1.1: keep-alive unless "Connection: close"
   - HTTP/1.0: close unless "Connection: keep-alive" */
static int connection_should_close(const struct myhttp_req *r) {
	int is_http10 = (r->version && strncmp(r->version, "HTTP/1.0", 8) == 0);
	if (r->h_connection) {
		if (strncasecmp(r->h_connection, "close", 5) == 0) return 1;
		if (strncasecmp(r->h_connection, "keep-alive", 10) == 0) return 0;
	}
	return is_http10 ? 1 : 0;
}

/* Serve a resolved absolute path (may be file or directory).
   Uses your fs_join_safe, fs_try_index, fs_open_ro, fs_mime_from_path, fs_send_dir_listing. */
static int serve_resolved_path(int client_fd, const char *docroot_real,
                               const char *decoded_path) {
	char abs[PATH_MAX];
	if (fs_join_safe(docroot_real, decoded_path, abs, sizeof(abs)) < 0) {
		if (errno == EACCES) return send_simple_response(client_fd, 403, "Forbidden", "forbidden\n");
		return send_simple_response(client_fd, 404, "Not Found", "not found\n");
	}

	int isdir = fs_is_dir(abs);
	if (isdir < 0) {
		if (errno == ENOENT) return send_simple_response(client_fd, 404, "Not Found", "not found\n");
		return send_simple_response(client_fd, 403, "Forbidden", "forbidden\n");
	}

	if (isdir == 1) {
		char indexed[PATH_MAX];
		int tri = fs_try_index(abs, "index.html", indexed, sizeof(indexed));
	if (tri == 1) {
		/* Found index.html -> serve it */
		int fd = fs_open_ro(indexed);
		if (fd < 0) return send_simple_response(client_fd, 403, "Forbidden", "forbidden\n");
		struct stat st; fstat(fd, &st);
		const char *mime = fs_mime_from_path(indexed);
		char hdr[512];
		int n = snprintf(hdr, sizeof(hdr),
				"HTTP/1.1 200 OK\r\n"
				"Content-Length: %zu\r\n"
				"Content-Type: %s\r\n"
				"Connection: keep-alive\r\n"
				"\r\n",
				(size_t)st.st_size, mime);
		if (n < 0 || (size_t)n >= sizeof(hdr)) { close(fd); return -1; }
		if (send(client_fd, hdr, (size_t)n, 0) < 0) { close(fd); return -1; }

		char buf[64 * 1024];
		ssize_t r;
		while ((r = read(fd, buf, sizeof(buf))) > 0) {
			ssize_t w = 0;
			while (w < r) {
				ssize_t s = send(client_fd, buf + w, (size_t)(r - w), 0);
				if (s <= 0) { close(fd); return -1; }
				w += s;
			}
		}
		close(fd);
		return (r < 0) ? -1 : 0;
	} else if (tri == 0) {
		/* No index -> directory listing. Send header (no len), then HTML via fs_send_dir_listing. */
		const char *hdr =
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/html; charset=utf-8\r\n"
			"Connection: close\r\n"
			"\r\n";
		if (send(client_fd, hdr, strlen(hdr), 0) < 0) return -1;

		/* Use requested path for display */
		const char *disp = (decoded_path && decoded_path[0]) ? decoded_path : "/";
            	(void)fs_send_dir_listing(client_fd, abs, disp);

            	return -2; /* signal caller to close connection */
        	} else {
            		return send_simple_response(client_fd, 500, "Internal Server Error", "index lookup failed\n");
        	}
    	}

	/* Regular file */
	int fd = fs_open_ro(abs);
	if (fd < 0) {
		if (errno == EACCES) return send_simple_response(client_fd, 403, "Forbidden", "forbidden\n");
		if (errno == EISDIR) return send_simple_response(client_fd, 403, "Forbidden", "directory\n");
		return send_simple_response(client_fd, 404, "Not Found", "not found\n");
	}

	struct stat st; fstat(fd, &st);
	const char *mime = fs_mime_from_path(abs);
	char hdr[512];
	int n = snprintf(hdr, sizeof(hdr),
			"HTTP/1.1 200 OK\r\n"
			"Content-Length: %zu\r\n"
			"Content-Type: %s\r\n"
			"Connection: keep-alive\r\n"
			"\r\n",
			(size_t)st.st_size, mime);
	if (n < 0 || (size_t)n >= sizeof(hdr)) { close(fd); return -1; }
	if (send(client_fd, hdr, (size_t)n, 0) < 0) { close(fd); return -1; }

	char b[64 * 1024];
	ssize_t r;
	while ((r = read(fd, b, sizeof(b))) > 0) {
		ssize_t w = 0;
		while (w < r) {
			ssize_t s = send(client_fd, b + w, (size_t)(r - w), 0);
			if (s <= 0) { close(fd); return -1; }
			w += s;
		}
	}
	close(fd);
	return (r < 0) ? -1 : 0;
}

/* Handle requests on one socket until the client (or server) closes. */
static void serve_client_socket(int cfd) {
    char buf[RECV_BUF_SZ];
    size_t used = 0;
    int force_close = 0;

    for (;;) {
        if (used < sizeof(buf)) {
            ssize_t n = recv(cfd, buf + used, sizeof(buf) - used, 0);
            if (n == 0) break;                     /* client closed */
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                break;
            }
            used += (size_t)n;
        }

        struct myhttp_req req;
        myhttp_req_reset(&req);
        req.buf = buf;
        req.buf_len = used;

        int consumed = myhttp_parse_request(buf, used, &req);
        if (consumed < 0) {
            (void)send_simple_response(cfd, 400, "Bad Request", "bad request\n");
            break;
        }
        if (consumed == 0) {
            if (used == sizeof(buf)) {
                (void)send_simple_response(cfd, 413, "Payload Too Large", "header too large\n");
                break;
            }
            continue; /* need more data */
        }

        long clen = myhttp_content_length(&req);
        int method = req.method;

        /* For body-carrying methods, handle Expect: 100-continue + ensure Content-Length present */
        if (method == MYHTTP_POST || method == MYHTTP_PUT || method == MYHTTP_PATCH) {
            if (myhttp_expect_100(&req)) {
                (void)send(cfd, "HTTP/1.1 100 Continue\r\n\r\n", 25, 0);
            }
            if (clen < 0) {
                (void)send_simple_response(cfd, 411, "Length Required", "length required\n");
                break;
            }
        }

        int rc = 0;

        switch (method) {
            case MYHTTP_GET: {
                char decoded[PATH_MAX];
                if (extract_decoded_path(&req, decoded, sizeof(decoded)) < 0) {
                    /* Compact pipelined data */
                    size_t remain = used - (size_t)consumed;
                    memmove(buf, buf + consumed, remain);
                    used = remain;
                    rc = send_simple_response(cfd, 400, "Bad Request", "bad target\n");
                    break;
                }
                /* No request body for GET (beyond headers). Compact and serve. */
                size_t remain = used - (size_t)consumed;
                memmove(buf, buf + consumed, remain);
                used = remain;

                rc = serve_resolved_path(cfd, g_docroot, decoded);
                if (rc == -2) { force_close = 1; rc = 0; } /* directory listing path—close after */
                break;
            }

            case MYHTTP_DELETE: {
                size_t remain = used - (size_t)consumed;
                memmove(buf, buf + consumed, remain);
                used = remain;
                rc = send_simple_response(cfd, 405, "Method Not Allowed", "DELETE disabled\n");
                break;
            }

            case MYHTTP_POST:
            case MYHTTP_PUT:
            case MYHTTP_PATCH: {
                if (clen < 0) {
                    rc = send_simple_response(cfd, 411, "Length Required", "length required\n");
                    break;
                }

                /* ---- NEW: compute pre-read body slice after end-of-headers ---- */
                size_t header_end = (size_t)consumed;            /* body starts here in buf */
                const void *prefill_ptr = NULL;
                size_t      prefill_len = 0;
                if (used > header_end) {
                    prefill_ptr = buf + header_end;
                    prefill_len = used - header_end;
                }
                if ((size_t)clen < prefill_len) {
                    /* Client claimed fewer bytes than already delivered: malformed */
                    rc = send_simple_response(cfd, 400, "Bad Request", "invalid Content-Length\n");
                    /* drop buffered data */
                    used = 0;
                    break;
                }

                char decoded[PATH_MAX];
                if (extract_decoded_path(&req, decoded, sizeof(decoded)) < 0) {
                    size_t remain = used - (size_t)consumed;
                    memmove(buf, buf + consumed, remain);
                    used = remain;
                    rc = send_simple_response(cfd, 400, "Bad Request", "bad target\n");
                    break;
                }

                /* For body methods we will hand off body to fs; after that, clear buf. */
                if (method == MYHTTP_PATCH) {
                    /* NOTE: PATCH currently ignores prefill bytes unless you add an
                       fs_append_from_socket_prefill(). For now, keep legacy behavior. */
                    used = 0; /* discard buffered bytes so append reads from socket */
                    if (fs_append_from_socket(g_docroot, decoded, cfd, (size_t)clen) == 0)
                        rc = send_simple_response(cfd, 204, "No Content", "");
                    else if (errno == EISDIR)
                        rc = send_simple_response(cfd, 409, "Conflict", "cannot append to directory\n");
                    else
                        rc = send_simple_response(cfd, 403, "Forbidden", "append failed\n");
                } else {
                    /* ---- NEW: prefill-aware atomic writer for PUT/POST ---- */
                    int w = fs_put_from_socket_atomic_prefill(
                                g_docroot,
                                decoded,
                                cfd,
                                (size_t)clen,
                                prefill_ptr,
                                prefill_len);

                    /* After fs drains body (prefill + remainder), clear buffer for next req */
                    used = 0;

                    if (w >= 0) {
                        rc = (w == 1)
                            ? send_simple_response(cfd, 201, "Created", "created\n")
                            : send_simple_response(cfd, 204, "No Content", "");
                    } else if (errno == EISDIR) {
                        rc = send_simple_response(cfd, 409, "Conflict", "target is directory\n");
                    } else if (errno == ENOENT) {
                        rc = send_simple_response(cfd, 404, "Not Found", "parent missing\n");
                    } else if (errno == EACCES || errno == EPERM) {
                        rc = send_simple_response(cfd, 403, "Forbidden", "permission denied\n");
                    } else if (errno == EPROTO) {
                        rc = send_simple_response(cfd, 400, "Bad Request", "invalid Content-Length\n");
                    } else {
                        rc = send_simple_response(cfd, 500, "Internal Server Error", "write failed\n");
                    }
                }
                break;
            }

            default: {
                size_t remain = used - (size_t)consumed;
                memmove(buf, buf + consumed, remain);
                used = remain;
                rc = send_simple_response(cfd, 405, "Method Not Allowed", "use GET\n");
                break;
            }
        }

        if (rc < 0) break;

        int close_conn = connection_should_close(&req) || force_close;
        if (used == 0 && close_conn) break;
        /* else: continue (maybe pipelined next request already in 'buf') */
    }
}
/* --------- Worker thread: pulls sockets from mh_workq --------- */

struct worker_args {
	struct mh_workq *q;
};

static void *worker_thread_main(void *arg) {
	struct worker_args *wa = (struct worker_args *)arg;
	struct mh_workq *q = wa->q;

	for (;;) {
		struct mh_job job;
		if (workq_dequeue(q, &job) != 0) {
			/* queue closed & empty */
			break;
		}
		/* Log peer info (optional) */
		char ip[INET6_ADDRSTRLEN] = {0};
		int port = 0;
		peer_to_str(&job.peer, ip, sizeof(ip), &port);
		fprintf(stderr, "[*] Worker handling %s:%d (fd=%d)\n", ip, port, job.client_fd);

		serve_client_socket(job.client_fd);
		close(job.client_fd);
	}

	return NULL;
}

/* --------- main() --------- */

int main(int argc, char *argv[]) {
	struct config cfg = { .port = 8080, .dir = "." };

	/* Parse args */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "Error: -p requires an argument\n"); usage(argv[0]); return 1; }
			int port = parse_int(argv[++i]);
			if (port <= 20) { fprintf(stderr, "Error: invalid port number\n"); usage(argv[0]); return 1; }
			cfg.port = port;
		} else if (strcmp(argv[i], "-d") == 0) {
			if (i + 1 >= argc) { fprintf(stderr, "Error: -d requires argument\n"); usage(argv[0]); return 1; }
			cfg.dir = argv[++i];
		} else {
			fprintf(stderr, "Error: unknown argument %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	/* Avoid SIGPIPE killing the process if peer closes */
	signal(SIGPIPE, SIG_IGN);

	/* Resolve docroot to an absolute, symlink-free path once */
	if (!realpath(cfg.dir, g_docroot)) {
		perror("realpath(docroot)");
		return 1;
	}

	printf("Starting MyHTTP…\n");
	printf("\t Port: %d\n", cfg.port);
	printf("\t Root: %s\n", g_docroot);

	/* Create listening socket */
	int sfd = socket(AF_INET6, SOCK_STREAM, 0);
	if (sfd < 0) {
		/* Fall back to IPv4 if IPv6 not available */
		sfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sfd < 0) { perror("socket"); return 1; }
		int one = 1;
		setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		struct sockaddr_in addr4;
		memset(&addr4, 0, sizeof(addr4));
		addr4.sin_family = AF_INET;
		addr4.sin_addr.s_addr = htonl(INADDR_ANY);
		addr4.sin_port = htons((uint16_t)cfg.port);

		if (bind(sfd, (struct sockaddr*)&addr4, sizeof(addr4)) < 0) { perror("bind"); close(sfd); return 1; }
	} else {
		int one = 1;
		setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		/* Dual-stack if possible */
		int off = 0; /* 0 = allow v4-mapped on v6 */
		setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

		struct sockaddr_in6 addr6;
		memset(&addr6, 0, sizeof(addr6));
		addr6.sin6_family = AF_INET6;
		addr6.sin6_addr = in6addr_any;
		addr6.sin6_port = htons((uint16_t)cfg.port);

		if (bind(sfd, (struct sockaddr*)&addr6, sizeof(addr6)) < 0) { perror("bind"); close(sfd); return 1; }
	}

	if (listen(sfd, BACKLOG) < 0) { perror("listen"); close(sfd); return 1; }
	fprintf(stderr, "Listening on port %d … (workers=%d)\n", cfg.port, N_WORKERS);

	/* Initialize work queue and start worker threads */
	struct mh_workq q;
	if (workq_init(&q, 1024) != 0) {
		fprintf(stderr, "workq_init failed: %s\n", strerror(errno));
		close(sfd);
		return 1;
	}

	pthread_t tids[N_WORKERS];
	struct worker_args wa = { .q = &q };
	for (int i = 0; i < N_WORKERS; i++) {
		if (pthread_create(&tids[i], NULL, worker_thread_main, &wa) != 0) {
			fprintf(stderr, "pthread_create failed (worker %d)\n", i);
		}
	}

	/* Accept loop: enqueue sockets for workers */
	for (;;) {
		struct sockaddr_storage peer;
		socklen_t plen = sizeof(peer);
		int cfd = accept(sfd, (struct sockaddr*)&peer, &plen);
		if (cfd < 0) {
			if (errno == EINTR) continue;
			perror("accept");
			continue;
		}

		char ip[INET6_ADDRSTRLEN] = {0};
		int port = 0;
		peer_to_str(&peer, ip, sizeof(ip), &port);
		fprintf(stderr, "[+] Connection from %s:%d (fd=%d)\n", ip, port, cfd);

		struct mh_job job;
		job.client_fd = cfd;
		job.peer = peer;
		job.peerlen = plen;

		if (workq_enqueue(&q, job) != 0) {
			fprintf(stderr, "workq_enqueue failed; dropping connection\n");
			close(cfd);
			continue;
		}
	}

	/* Not normally reached; graceful shutdown pattern for completeness */
	workq_close(&q);
	for (int i = 0; i < N_WORKERS; i++) pthread_join(tids[i], NULL);
	workq_destroy(&q);
	close(sfd);
	return 0;
}
