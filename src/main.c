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

	/* TODO init server and listening socket here and handle connections */

	// --- Added: minimal TCP server that prints everything received ---

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
		for (;;) {
			ssize_t n = recv(cfd, buf, sizeof(buf), 0);
			if (n == 0) break;                  // client closed
			if (n < 0) {
				if (errno == EINTR) continue;
				perror("recv");
				break;
			}
			// Write raw bytes to stdout
			ssize_t off = 0;
			while (off < n) {
				ssize_t m = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
				if (m < 0) {
					if (errno == EINTR) continue;
					perror("write(stdout)");
					goto done_client;
				}
				off += m;
			}
			fflush(stdout);
		}
	done_client:
		fprintf(stderr, "[-] Disconnect %s:%d\n", ip, pport);
		close(cfd);
	}

	// not reached
	close(sfd);
	return 0;
}

