#include "http_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>           // close, write
#include <signal.h>           // signal, SIGPIPE
#include <arpa/inet.h>        // inet_ntop, htons, htonl
#include <netinet/in.h>       // sockaddr_in
#include <sys/socket.h>       // socket, bind, listen, accept, recv

struct config { // What we use to set the port and directory to read from.
	 int port;
	 const char *dir;
};

static void usage(const char *prog) { // Usage string for -h
    fprintf(stderr, "Usage: %s [-p port] [-d root]\n", prog);
    fprintf(stderr, "Defaults: port=8080, root='.'\n");
}

// Helper function to parse Integers.
static int parse_int(const char *s) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s[0] || (end && *end != '\0') || v < 1 || v > 65535)
        return -1;
    return (int)v;
}

static int send_simple_response(int fd, int code, const char *reason,
                                const char *body) {
    char hdr[256];
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        code, reason, blen);
    if (n < 0 || (size_t)n >= sizeof(hdr)) return -1;

    ssize_t w = send(fd, hdr, (size_t)n, 0);
    if (w < 0) return -1;
    if (blen) {
        w = send(fd, body, blen, 0);
        if (w < 0) return -1;
    }
    return 0;
}

/* Discard exactly 'need' body bytes, using what's already in buf after 'consumed'.
   Adjusts *used and compacts any extra (pipelined) bytes back to the front. */
static int discard_body_and_compact(int fd,
                                    char *buf, size_t *used,
                                    size_t consumed, size_t need) {
    size_t after = *used - consumed;        // bytes after headers currently in buf
    if (after >= need) {
        // We already have the whole body (and maybe the start of next request)
        size_t remain = after - need;       // pipelined bytes after the body
        memmove(buf, buf + consumed + need, remain);
        *used = remain;
        return 0;
    }
    // Consume what we have, drop the rest from the socket
    *used = 0; // buffer becomes empty while we drain the rest
    size_t left = need - after;
    char scratch[16 * 1024];
    while (left > 0) {
        size_t chunk = left < sizeof(scratch) ? left : sizeof(scratch);
        ssize_t n = recv(fd, scratch, chunk, 0);
        if (n <= 0) return -1;  // client closed or error while reading body
        left -= (size_t)n;
    }
    return 0;
}

// MAIN FUNCTION:
// parse -p, -d. Default: port 8080, root “.”.
int main(int argc, char *argv[]) {
	struct config cfg = {
		.port = 8080,
		.dir = ".",
	};

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0) {
			if (i + 1 >= argc) {
                		fprintf(stderr, "Error: -p requires an argument\n");
                		usage(argv[0]);
                		return 1;
            		}
			int port = parse_int(argv[++i]);
			if (port <= 20) {
				fprintf(stderr, "Error: invalid port number\n");
				usage(argv[0]);
				return 1;
			}
			cfg.port = port;
		} else if (strcmp(argv[i], "-d") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Error: -d requires argument\n");
				usage(argv[0]);
				return 1;
			}
			cfg.dir = argv[++i];
		} else {
			fprintf(stderr, "Error: unknown argument %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}
	printf("Starting MyHTTP...\n");
	printf("\t Port: %d\n", cfg.port);
	printf("\t Root: %s\n", cfg.dir);

	// Avoid SIGPIPE killing the process if stdout/peer closes.
	signal(SIGPIPE, SIG_IGN);

	// Create listening socket (IPv4, TCP)
	int sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0) {
		perror("socket");
		return 1;
	}

	// Reuse address to avoid "address already in use" after restarts
	int one = 1;
	if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
		perror("setsockopt(SO_REUSEADDR)");
		close(sfd);
		return 1;
	}

	// Bind to 0.0.0.0:port
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)cfg.port);

	if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(sfd);
		return 1;
	}

	// Listen
	if (listen(sfd, 128) < 0) {
		perror("listen");
		close(sfd);
		return 1;
	}

	fprintf(stderr, "Listening on 0.0.0.0:%d …\n", cfg.port);

	// Accept loop: handle one client at a time; print all bytes received.
	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int cfd = accept(sfd, (struct sockaddr*)&peer, &plen);
		if (cfd < 0) {
			if (errno == EINTR) continue;
			perror("accept");
			continue;
		}

		char ip[INET_ADDRSTRLEN] = {0};
		inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		int pport = ntohs(peer.sin_port);
		fprintf(stderr, "[+] Connection from %s:%d\n", ip, pport);

		char buf[64 * 1024];
    		size_t used = 0;
		
		for (;;) {
			if (used < sizeof(buf)) {
            			ssize_t n = recv(cfd, buf + used, sizeof(buf) - used, 0);
            			if (n == 0) break;                  // client closed
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
			// fwrite(buf, 1, used, stdout);
			// fflush(stdout);// PRINT OUT WHAT WE GET
        		if (consumed < 0) {
            			// Malformed request
            			(void)send_simple_response(cfd, 400, "Bad Request", "bad request\n");
            			break; // close connection on parse error
        		}
        		if (consumed == 0) {
            			// Need more data; if buffer is full, reject
            			if (used == sizeof(buf)) {
                			(void)send_simple_response(cfd, 413, "Payload Too Large", "header too large\n");
                			break;
            			}
            			continue; // read more
        		}
	        	// We have headers. Handle Expect: 100-continue for body methods.
        		long clen = myhttp_content_length(&req);
        		int method = req.method;
        		if ((method == MYHTTP_POST || method == MYHTTP_PUT || method == MYHTTP_PATCH)) {
            			if (myhttp_expect_100(&req)) {
                			(void)send(cfd, "HTTP/1.1 100 Continue\r\n\r\n", 25, 0);
            			}
            			if (clen < 0) {
                			(void)send_simple_response(cfd, 411, "Length Required", "length required\n");
			                // consume nothing beyond headers; close
                			break;
            			}
        		}
 			/* ---- route minimal semantics for now ---- */
        		int rc = 0;
        		switch (method) {
        		case MYHTTP_GET: {
            			// MVP: just respond OK (you'll hook real file serving later)
            			// No body to consume
            			// Compact any pipelined bytes already in buffer
            			size_t remain = used - (size_t)consumed;
           			memmove(buf, buf + consumed, remain);
            			used = remain;
            			rc = send_simple_response(cfd, 200, "OK", "GET ok\n");
            			break;
        		}
        		case MYHTTP_POST:
        		case MYHTTP_PUT:
        		case MYHTTP_PATCH: {
            			if (clen < 0) { 
					rc = send_simple_response(cfd, 411, "Length Required", "length required\n");
					break;
				}
            			if (discard_body_and_compact(cfd, buf, &used, (size_t)consumed, (size_t)clen) < 0) {
                			rc = -1; break;
            			}
            			// Minimal replies (tweak later for your file semantics)
            			if (method == MYHTTP_POST) 	rc = send_simple_response(cfd, 201, "Created", "POST created\n");
				else if (method == MYHTTP_PUT) 	rc = send_simple_response(cfd, 200, "OK", "PUT ok\n");
				else /*PATCH*/			rc = send_simple_response(cfd, 200, "OK", "PATCH ok\n");
            			break;
			}
        		case MYHTTP_DELETE: {
            			// No request body for MVP; just respond OK
            			size_t remain = used - (size_t)consumed;
            			memmove(buf, buf + consumed, remain);
            			used = remain;
            			rc = send_simple_response(cfd, 200, "OK", "DELETE ok\n");
            			break;
        		}
        		default: {
            			// Method not allowed
            			size_t remain = used - (size_t)consumed;
            			memmove(buf, buf + consumed, remain);
            			used = remain;
           			rc = send_simple_response(cfd, 405, "Method Not Allowed", "use GET/POST/PUT/PATCH/DELETE\n");
            			break;
        		}
        		}
        		if (rc < 0) {
            			perror("send");
            			break;
        		}
			fflush(stdout); // FLUSH OUT ANY REMAINING STUFF
		}
		fprintf(stderr, "[-] Disconnect %s:%d\n", ip, pport);
		close(cfd);
	}
	// not reached
	close(sfd);
	return 0;
}

