#define _POSIX_C_SOURCE 200809L

#include "pathlock.h"
#include <sys/types.h>
#include <sys/socket.h>   // recv()
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h> 

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MATCH(x) (strcmp(ext, (x)) == 0) // MACRO FOR COMPARING STRINGS

static int write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char*)buf;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

static const char* fs_get_ext_lower(const char *path, char *extbuf, size_t extbuflen) {
    if (!path || !extbuf || extbuflen == 0) { return NULL; }

    const char *dot = strrchr(path, '.');     // find last '.'
    if (!dot || dot[1] == '\0') { return NULL; }

    // Copy extension after '.'
    size_t i = 0;
    for (const char *p = dot + 1; *p && i + 1 < extbuflen; ++p) {
        extbuf[i++] = (char)tolower((unsigned char)*p);
    }
    extbuf[i] = '\0';
    return extbuf;
}

int fs_join_safe(const char *docroot_real, const char *decoded_req_path,
                 char *out, size_t outlen)
{
    if (!docroot_real || !out || outlen == 0) {
        errno = EINVAL; return -1;
    }
    if (docroot_real[0] != '/') {
        errno = EINVAL; return -1;
    }

    if (!decoded_req_path) decoded_req_path = "";

    /* ---- Normalize into relative components (no "", ".", or "..") ---- */
    const char *p = decoded_req_path;
    while (*p == '/') p++;  /* skip leading slashes to make it relative */

    size_t comp_cap = strlen(p) + 1;
    char **comps = comp_cap ? (char **)calloc(comp_cap, sizeof(char*)) : NULL;
    if (comp_cap && !comps) { errno = ENOMEM; return -1; }

    char *path_copy = strdup(p ? p : "");
    if (!path_copy) { free(comps); errno = ENOMEM; return -1; }

    size_t ncomps = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(path_copy, "/", &saveptr);
         tok != NULL;
         tok = strtok_r(NULL, "/", &saveptr))
    {
        if (tok[0] == '\0' || (tok[0] == '.' && tok[1] == '\0')) {
            continue; /* skip "" and "." */
        }
        if (tok[0] == '.' && tok[1] == '.' && tok[2] == '\0') {
            /* ".." -> pop if possible, else reject */
            if (ncomps == 0) { free(comps); free(path_copy); errno = EINVAL; return -1; }
            ncomps--;
            continue;
        }
        comps[ncomps++] = tok; /* pointer into path_copy storage */
    }

    /* ---- Build candidate absolute path "cand" = docroot_real + '/' + comps ---- */
    size_t doclen = strlen(docroot_real);

    size_t rel_len = 0;
    for (size_t i = 0; i < ncomps; i++) {
        rel_len += (i ? 1 : 0) + strlen(comps[i]); /* +1 for '/' between comps */
    }
    int rel_is_dot = (ncomps == 0);

    /* need = docroot + (optional '/' + rel) + NUL */
    size_t need = doclen + (rel_is_dot ? 0 : 1 + rel_len) + 1;
    char *cand = (char *)malloc(need);
    if (!cand) { free(comps); free(path_copy); errno = ENOMEM; return -1; }

    /* Populate cand */
    size_t pos = 0;
    memcpy(cand + pos, docroot_real, doclen);
    pos += doclen;

    if (!rel_is_dot) {
        cand[pos++] = '/';
        for (size_t i = 0; i < ncomps; i++) {
            size_t sl = strlen(comps[i]);
            memcpy(cand + pos, comps[i], sl);
            pos += sl;
            if (i + 1 < ncomps) cand[pos++] = '/';
        }
    }
    cand[pos] = '\0';

    /* ---- Find deepest existing directory parent under/at docroot_real ---- */
    struct stat st;
    char *parent = NULL;
    char *tail   = NULL;

    char *probe = strdup(cand); /* we’ll insert temporary NULs */
    if (!probe) { free(cand); free(comps); free(path_copy); errno = ENOMEM; return -1; }

    size_t min_prefix = doclen;               /* never go above docroot_real */
    ssize_t i = (ssize_t)strlen(probe);
    int found = 0;

    while (i >= (ssize_t)min_prefix) {
        if (i == (ssize_t)strlen(probe) || probe[i] == '/') {
            char saved = probe[i];
            probe[i] = '\0';

            if (probe[0] == '\0') {           /* shouldn’t happen (absolute) */
                probe[i] = saved;
                break;
            }
            if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
                parent = strdup(probe);
                probe[i] = saved;
                if (!parent) { free(probe); free(cand); free(comps); free(path_copy); errno = ENOMEM; return -1; }

                /* Compute tail after this parent */
                if ((size_t)i < strlen(cand)) {
                    size_t start = (saved == '/') ? (size_t)i + 1 : (size_t)i;
                    tail = strdup(cand + start);
                } else {
                    tail = strdup("");
                }
                if (!tail) { free(parent); free(probe); free(cand); free(comps); free(path_copy); errno = ENOMEM; return -1; }

                found = 1;
                break;
            }
            probe[i] = saved;
        }
        i--;
    }

    if (!found) {
        /* Fall back to docroot_real as parent if it exists and is a directory */
        if (stat(docroot_real, &st) == 0 && S_ISDIR(st.st_mode)) {
            parent = strdup(docroot_real);
            if (!parent) { free(probe); free(cand); free(comps); free(path_copy); errno = ENOMEM; return -1; }
            if ((size_t)doclen < strlen(cand)) {
                size_t start = (cand[doclen] == '/') ? doclen + 1 : doclen;
                tail = strdup(cand + start);
            } else {
                tail = strdup("");
            }
            if (!tail) { free(parent); free(probe); free(cand); free(comps); free(path_copy); errno = ENOMEM; return -1; }
            found = 1;
        } else {
            /* docroot must exist & be a directory */
            free(probe); free(cand); free(comps); free(path_copy);
            errno = ENOENT; return -1;
        }
    }

    /* ---- Canonicalize existing parent with realpath (resolves symlinks) ---- */
    char canon_parent[PATH_MAX];
    if (!realpath(parent, canon_parent)) {
        int saved = errno;
        free(parent); free(tail); free(probe); free(cand); free(comps); free(path_copy);
        errno = saved ? saved : EINVAL;
        return -1;
    }

    /* ---- Ensure resolved parent is inside docroot_real (no symlink escape) ---- */
    size_t dl = strlen(docroot_real);
    if (!(strncmp(canon_parent, docroot_real, dl) == 0 &&
          (canon_parent[dl] == '\0' || canon_parent[dl] == '/')))
    {
        free(parent); free(tail); free(probe); free(cand); free(comps); free(path_copy);
        errno = EACCES; /* outside of docroot via symlinked parent */
        return -1;
    }

    /* ---- Build final absolute path into 'out' ---- */
    size_t cp_len   = strlen(canon_parent);
    size_t tail_len = strlen(tail);
    size_t final_len = cp_len + (tail_len ? 1 + tail_len : 0);

    if (final_len + 1 > outlen) {
        free(parent); free(tail); free(probe); free(cand); free(comps); free(path_copy);
        errno = ENAMETOOLONG; return -1;
    }

    char *dst = out;
    memcpy(dst, canon_parent, cp_len);
    dst += cp_len;
    if (tail_len) {
        *dst++ = '/';
        memcpy(dst, tail, tail_len);
        dst += tail_len;
    }
    *dst = '\0';

    /* ---- Cleanup & success ---- */
    free(parent); free(tail); free(probe); free(cand); free(comps); free(path_copy);
    return 0;
}

int fs_is_dir(const char *abs_path) {
    	if (!abs_path) {
       		errno = EINVAL;
        	return -1;
    	}
    	struct stat st;
    	if (stat(abs_path, &st) == -1) {
        	return -1;
    	}
    	if (S_ISDIR(st.st_mode)) {
        	return 1;   // it's a directory
    	}
    	return 0;       // exists, but not a directory
}

int fs_try_index(const char *dir_abs, const char *index_name, char *out, size_t outlen) {
    	// VALIDATES INPUTS
    	if (!dir_abs || !index_name || !out || outlen == 0) { errno = EINVAL; return -1; }
    	if (dir_abs[0] != '/') { // must be absolute
        	errno = EINVAL; return -1; }
    	if (index_name[0] == '\0' || strchr(index_name, '/')) { errno = EINVAL; return -1; }

    	// Confirm dir_abs is a directory
    	int d = fs_is_dir(dir_abs);
    	if (d == -1) return -1; // errno set by fs_is_dir (e.g., ENOENT/EACCES)
    	if (d == 0) { errno = ENOTDIR; return -1; }

    	// BUILD CANIDATE
    	size_t dlen = strlen(dir_abs);
    	size_t ilen = strlen(index_name);
    	bool need_slash = (dlen == 0 || dir_abs[dlen - 1] != '/');
    	size_t cand_len = dlen + (need_slash ? 1 : 0) + ilen;

    	char *cand = (char *)malloc(cand_len + 1);
    	if (!cand) { errno = ENOMEM; return -1; }

    	char *p = cand;
    	memcpy(p, dir_abs, dlen); p += dlen;
    	if (need_slash) *p++ = '/';
    	memcpy(p, index_name, ilen); p += ilen;
    	*p = '\0';

    	// CHECK STATUS OF CANIDATE
    	struct stat st;
    	if (stat(cand, &st) == -1) {
        	int saved = errno;
        	free(cand);
        	if (saved == ENOENT) {
            		return 0;
        	}
        	errno = saved;
        	return -1;
    	}

    	if (!S_ISREG(st.st_mode)) {
        	// Present but not a regular file (treat as "not found" for an index)
        	free(cand);
	        return 0;
	}

    	// COPY ABSOLUTE PATH TO OUT
    	if (cand_len + 1 > outlen) {
        	free(cand);
        	errno = ENAMETOOLONG;
        	return -1;
    	}
    	memcpy(out, cand, cand_len + 1);
    	free(cand);
    	return 1;
}

int fs_open_ro(const char *abs_path) {
	if (!abs_path) { errno = EINVAL; return -1; }

	int fd = open(abs_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd == -1) { 
		return -1;
	}

	struct stat st;
	if (fstat(fd, &st) == -1) {
		close(fd);
		return -1;
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		errno = EISDIR;
		return -1;
	}
	return fd;
}

/* Basic HTML escape for names in dir listing */
static void html_escape(const char *s, char *out, size_t outlen) {
    size_t w = 0;
    for (; *s && w + 6 < outlen; ++s) {
        unsigned char c = (unsigned char)*s;
        if      (c == '&')  { memcpy(out + w, "&amp;", 5);  w += 5; }
        else if (c == '<')  { memcpy(out + w, "&lt;", 4);   w += 4; }
        else if (c == '>')  { memcpy(out + w, "&gt;", 4);   w += 4; }
        else if (c == '"')  { memcpy(out + w, "&quot;", 6); w += 6; }
        else out[w++] = (char)c;
    }
    out[w] = '\0';
}

const char* fs_mime_from_path(const char *abs_path) {
	if (!abs_path) return "application/octet-stream";

    	char ext[16];
    	const char *e = fs_get_ext_lower(abs_path, ext, sizeof(ext));
    	if (!e) return "application/octet-stream";
	
	// USING MATCH MACRO FOR READABILITY
	if (MATCH("html") || MATCH("htm")) return "text/html";
    	if (MATCH("css"))  return "text/css";
	if (MATCH("js"))   return "application/javascript";
    	if (MATCH("json")) return "application/json";
    	if (MATCH("png"))  return "image/png";
    	if (MATCH("jpg") || MATCH("jpeg")) return "image/jpeg";
    	if (MATCH("gif"))  return "image/gif";
    	if (MATCH("svg"))  return "image/svg+xml";
    	if (MATCH("ico"))  return "image/x-icon";
    	if (MATCH("txt"))  return "text/plain; charset=utf-8";
    	if (MATCH("pdf"))  return "application/pdf";
    	// DEFAULT
	return "application/octet-stream";
}

int fs_send_dir_listing(int client_fd, const char *dir_abs, const char *req_path_display) {
    DIR *dir = opendir(dir_abs);
    if (!dir) return -1;

    const char *hdr =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of ";
    const char *mid = "</title></head><body><h1>Index of ";
    const char *ul  = "</h1><ul>\n";

    char esc[PATH_MAX];
    html_escape((req_path_display && *req_path_display) ? req_path_display : "/", esc, sizeof(esc));

    char head[1024];
    int n = snprintf(head, sizeof(head), "%s%s%s%s", hdr, esc, mid, esc);
    if (n < 0 || (size_t)n >= sizeof(head)) { closedir(dir); errno = ENOMEM; return -1; }
    if (write_all(client_fd, head, (size_t)n) < 0) { int e=errno; closedir(dir); errno=e; return -1; }
    if (write_all(client_fd, ul, strlen(ul)) < 0)  { int e=errno; closedir(dir); errno=e; return -1; }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char child_abs[PATH_MAX];
        snprintf(child_abs, sizeof(child_abs), "%s%s%s",
                 dir_abs, (dir_abs[strlen(dir_abs)-1] == '/' ? "" : "/"), name);

        struct stat st_child;
        int is_dir = (stat(child_abs, &st_child) == 0 && S_ISDIR(st_child.st_mode));

        char escname[PATH_MAX];
        html_escape(name, escname, sizeof(escname));

        char line[PATH_MAX + 128];
        int m = snprintf(line, sizeof(line),
                         "<li><a href=\"%s%s%s\">%s%s</a></li>\n",
                         esc,
                         (esc[strlen(esc)-1] == '/' ? "" : "/"),
                         escname,
                         escname,
                         (is_dir ? "/" : ""));
        if (m < 0 || (size_t)m >= sizeof(line)) continue;
        if (write_all(client_fd, line, (size_t)m) < 0) { int e=errno; closedir(dir); errno=e; return -1; }
    }

    const char *footer = "</ul></body></html>\n";
    write_all(client_fd, footer, strlen(footer));
    closedir(dir);
    return 0;
}

static int copy_exact_from_sock(int sock_fd, int dst_fd, size_t len) {
    char buf[64 * 1024];
    size_t left = len;
    while (left) {
        size_t want = left < sizeof(buf) ? left : sizeof(buf);
        ssize_t r = recv(sock_fd, buf, want, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            errno = (r == 0) ? EIO : errno;
            return -1;
        }
        if (write_all(dst_fd, buf, (size_t)r) < 0) return -1;
        left -= (size_t)r;
    }
    return 0;
}

static int mktemp_in_samedir(char *tmp_out, size_t outlen, const char *final_abs) {
    const char *slash = strrchr(final_abs, '/');
    if (!slash) { errno = EINVAL; return -1; }
    size_t dirlen = (size_t)(slash - final_abs);
    if (dirlen + strlen("/.myhttp.XXXXXX") + 1 > outlen) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp_out, final_abs, dirlen);
    memcpy(tmp_out + dirlen, "/.myhttp.XXXXXX", sizeof("/.myhttp.XXXXXX"));
    return 0;
}

int fs_put_from_socket_atomic(const char *docroot_real,
                              const char *decoded_req_path,
                              int client_fd, size_t content_len)
{
    char abs[PATH_MAX];
    if (fs_join_safe(docroot_real, decoded_req_path, abs, sizeof(abs)) < 0) return -1;

    if (plock_acquire_wr(abs) != 0) return -1;

    int rc = -1;
    int existed = 0;
    struct stat st;
    if (stat(abs, &st) == 0) existed = 1;

    if (existed && S_ISDIR(st.st_mode)) { errno = EISDIR; goto out_unlock; }

    char tmpl[PATH_MAX];
    if (mktemp_in_samedir(tmpl, sizeof(tmpl), abs) < 0) goto out_unlock;

    int tmpfd = mkstemp(tmpl);
    if (tmpfd < 0) goto out_unlock;

    if (copy_exact_from_sock(client_fd, tmpfd, content_len) < 0) {
        int e = errno; close(tmpfd); unlink(tmpl); errno = e; goto out_unlock;
    }

    if (fsync(tmpfd) < 0) {
        int e = errno; close(tmpfd); unlink(tmpl); errno = e; goto out_unlock;
    }
    if (close(tmpfd) < 0) {
        int e = errno; unlink(tmpl); errno = e; goto out_unlock;
    }

    if (rename(tmpl, abs) < 0) {
        int e = errno; unlink(tmpl); errno = e; goto out_unlock;
    }

    rc = existed ? 0 : 1;

out_unlock:
    plock_release(abs);
    return rc;
}

int fs_append_from_socket(const char *docroot_real,
                          const char *decoded_req_path,
                          int client_fd, size_t content_len)
{
    char abs[PATH_MAX];
    if (fs_join_safe(docroot_real, decoded_req_path, abs, sizeof(abs)) < 0) return -1;

    if (plock_acquire_wr(abs) != 0) return -1;

    int fd = open(abs, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) { int e = errno; plock_release(abs); errno = e; return -1; }

    int ok = copy_exact_from_sock(client_fd, fd, content_len);
    int e  = ok == 0 ? 0 : errno;
    if (ok == 0) (void)fsync(fd);
    close(fd);
    plock_release(abs);
    if (ok != 0) { errno = e; return -1; }
    return 0;
}

int fs_unlink_safe(const char *docroot_real, const char *decoded_req_path)
{
    char abs[PATH_MAX];
    if (fs_join_safe(docroot_real, decoded_req_path, abs, sizeof(abs)) < 0) return -1;

    if (plock_acquire_wr(abs) != 0) return -1;

    struct stat st;
    if (stat(abs, &st) == 0 && S_ISDIR(st.st_mode)) {
        plock_release(abs);
        errno = EISDIR;
        return -1;
    }

    int r = unlink(abs);
    int e = (r == 0) ? 0 : errno;
    plock_release(abs);
    if (r != 0) { errno = e; return -1; }
    return 0;
}

