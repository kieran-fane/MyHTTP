#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#define MATCH(x) (strcmp(ext, (x)) == 0) // MACRO FOR COMPARING STRINGS

static int send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EPIPE; return -1; }
        buf += (size_t)n;
        len -= (size_t)n;
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

static int is_abs_prefix_dir(const char *parent, const char *root) {
	size_t rl = strlen(root);
	if (strncmp(parent, root, rl) != 0) return 0;
	if (parent[rl] == '\0') return 1;            // exact match
	if (parent[rl] == '/')  return 1;            // boundary match
	return 0;                                    // e.g., "/var/www2" vs "/var/www"
}

int fs_join_safe(const char *docroot_real, const char *decoded_req_path, char *out, size_t outlen) {
	if (!docroot_real ||!out || outlen == 0) {
		errno = EINVAL;
		return -1;
	}
	if (docroot_real[0] != '/') {
		errno = EINVAL;
		return -1;
	}

	if (!decoded_req_path) {
		decoded_req_path = "";
	}

	// NORMALIZE into relative path
	const char *p = decode_req_path;
	while (*p == "/") { p++; }

	size_t comp_cap = strlen(p) + 1;
	char **comps = comp_cap ? (char **)calloc(comp_cap, size_of(char*)) : NULL;

	if (comp_cap && !comps) {errno = ENOMEM; return -1;}

	// We need a writeable copy of the path to place NUL Terminatores between components
	char *path_copy = strdup(p ? p : "");

	if (!path_copy) {free(comps); errno = ENOMEM; return -1;}

	size_t ncomps = 0;
	char *saveptr = NULL;

	for (char *tok = strtok_r(path_copy, "/", &saveptr); tok != NULL; 
			tok = strtok_r(NULL, "/" &saveptr)) {

		if (tok[0] == '\0' || (tok[0] == '.' && tok[1] == '\0')) {
			continue; // SKIPS "" and "."
		}
		 if (tok[0] == '.' && tok[1] == '.' && tok[2] == '\0') {
           	 	// ".." -> pop if possible, else reject (attempt to escape)
            		if (ncomps == 0) { free(comps); free(path_copy); errno = EINVAL; return -1; }
            		ncomps--;
            		continue;
        	}
        	comps[ncomps++] = tok; // keep pointer into path_copy storage
	}

	// BUILD THE CANIDATE LOCATION
	size_t doclen = strlen(docroot_real);

	size_t rel_len = 0;

	for (size_t i = 0; i < ncomps; i++) {
		// +1 for '/' between componets for i > 0.
		rel_len += (i ? 1 : 0) + strlen(comps[i]);

	}

	int rel_is_dot = (ncomps == 0);
	
	// total canidate length + '/' maybe + rel_len + null. 
	size_t need = doclen + (rel_is_dot ? 0 : 1 + rel_len) + 1;

	char *cand = (char *)malloc(need);
	
	if (!cand) {free(comps); free(path_copy); errno = ENOMEM; return -1;}
	
	// Populate the canidate
	memcpy(cand, docroot_real, doclen);

	size_t pos = doc_len;

	if (!rel_is_dot) {
		cand[pos++] = '/';
		for (size_t i = 0; i < ncomps; i++) {
			size_t sl = strlen(comps[i]);
			memcpy(cand + pos, comps[i], sl); // Copying each directory over.
			pos += sl;
			if (i + 1 < ncomps) {cand[pos++] = '/';}
		}
	}
	cand[pos] = '\0';
	struct stat st; // To see if path is valid
	char *parent = NULL;
	char *tail = NULL;

	// Copy to protect data and place nulls to test prefixes.
	char *probe = strdup(cand);
	if (!probe) {free(cand); free(comps); free(path_copy); errno = ENOMEM; return -1;}

	// Go from full path, if it exists then we can default parent = cand and tail = ""
	// otherwise we go back until we find a path that actually exists and won't go passed 
	// docroot_real.
	size_t min_prefix = doclen;
	ssize_t i = (ssize_t)strlen(probe);

	int found = 0;

	while (i >= (ssize_t)min_prefix) {
		if (i == (ssize_t)strlen(probe) || probe[i] == '/') {
			char saved = probe[i];
			probe[i] = '\0';
			if (probe[0] == '\0') { // SHOULDN'T HAPPEN (ABSOLUTE PATHING)
				probe[i] == saved;
				break;
			}
			if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
				parent = strdup(probe);
				probe[i] = saved;
				if (!parent) { free(probe); free(cand); free(comps); free(path_copy);
					errno = ENOMEM; return -1;
				}
				// tail is the remainder after '/'
				if ((size_t)i < strlen(cand)) {
					// IF saved was '/', skip it for tail
					size_t start = (saved == '/') ? (size_t)i + 1 : (size_t)i;
					tail = strdup(cand + start);
					if (!tail) { free(parent); free(probe); free(cand); 
						free(comps); free(path_copy); 
						errno = ENOMEM; return -1;
					}
				} else {
					tail = strdup(""); // NOTHING LEFT
					if (!tail) { free(parent); free(probe); free(cand); 
						free(comps); free(path_copy); 
						errno = ENOMEM; return -1;
					}	
				}
				found = 1;
				break;
			}
		}
		i--;
	}
	if (!found) {
		 if (stat(docroot_real, &st) == 0 && S_ISDIR(st.st_mode)) {
            		parent = strdup(docroot_real);
            		if (!parent) {
				free(probe); free(cand); free(comps); free(path_copy); 
				errno = ENOMEM; return -1;
			}
            		// tail is everything after docroot_real + '/'
           		if ((size_t)doclen < strlen(cand)) {
                		size_t start = (cand[doclen] == '/') ? doclen + 1 : doclen;
               			tail = strdup(cand + start);
            		} else {
                		tail = strdup("");
            		}
            		if (!tail) { free(parent); free(probe); free(cand); free(comps); 
				free(path_copy); errno = ENOMEM; return -1;
			}
            		found = 1;
        	} else {
            		// docroot must exist and be a directory per contract
            		free(probe); free(cand); free(comps); free(path_copy);
            		errno = ENOENT;
            		return -1;
        	}
    	}
	
	// CANONICALIZE EXISTING PARENT WITH REALPATH
	char canon_parent[PATH_MAX];
    	if (!realpath(parent, canon_parent)) {
        	int saved = errno;
        	free(parent); free(tail); free(probe); free(cand); free(comps); free(path_copy);
        	errno = saved ? saved : EINVAL;
        	return -1;
    	}
	
	// CHECK THAT WE CAN ACCESS THIS
	if (!is_abs_prefix_dir(canon_parent, docroot_real)) {
        	free(parent); free(tail); free(probe); free(cand); free(comps); free(path_copy);
       		errno = EACCES;                           // outside of docroot via symlinked parent
        	return -1;
    	}

	// PUT FINAL PATH IN OUT
	size_t cp_len = strlen(canon_parent);
    	size_t tail_len = strlen(tail);
    	size_t final_len = cp_len + (tail_len ? 1 + tail_len : 0);

    	if (final_len + 1 > outlen) {
        	free(parent); free(tail); free(probe); free(cand); free(comps); free(path_copy);
       		errno = ENAMETOOLONG;
        	return -1;
    	}

    	// Write result
    	char *dst = out;
    	memcpy(dst, canon_parent, cp_len);
    	dst += cp_len;
    	if (tail_len) {
        	*dst++ = '/';
        	memcpy(dst, tail, tail_len);
        	dst += tail_len;
    	}
    	*dst = '\0';
	
	// CLEAN UP AND SUCCESS
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

	int fd = open(abs_path, O_READONLY | O_CLOEXEC | O_NOFOLOW);

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

// THIS FUNCTION IS A WAY TO SEE THE UNDERLYING FILESYSTEM IN A WEB-BROWSER FROM A HTTP REQUEST
// ITS PURPOSE IS DISPLAY THE FILESYSTEM BACK TO A CLIENT IN HTML.
int fs_send_dir_listing(int client_fd, const char *dir_abs, const char *req_path_display) {
    if (!dir_abs || !req_path_display) {
        errno = EINVAL;
        return -1;
    }

    int isdir = fs_is_dir(dir_abs);
    if (isdir == -1) return -1;
    if (isdir == 0) {
        errno = ENOTDIR;
        return -1;
    }

    DIR *dir = opendir(dir_abs);
    if (!dir) return -1;

    // HTML HEADER
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Index of %s</title></head><body><h1>Index of %s</h1><ul>\n",
        req_path_display, req_path_display);
    if (n < 0 || send_all(client_fd, header, (size_t)n) == -1) {
        int saved = errno;
        closedir(dir);
        errno = saved;
        return -1;
    }

    // PARENT DIRECTORY LINKS
    if (strcmp(req_path_display, "/") != 0) {
        const char *parent = "<li><a href=\"../\">../</a></li>\n";
        if (send_all(client_fd, parent, strlen(parent)) == -1) {
            int saved = errno;
            closedir(dir);
            errno = saved;
            return -1;
        }
    }

    // DIRECTORY ENTRIES 
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;

        // Skip "." and ".."
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        // Check if entry is directory
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir_abs, name);
        struct stat st;
        int is_dir_entry = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));

        // Write entry line
        char line[1024];
        n = snprintf(line, sizeof(line),
                     "<li><a href=\"%s%s%s\">%s%s</a></li>\n",
                     req_path_display,
                     (req_path_display[strlen(req_path_display) - 1] == '/' ? "" : "/"),
                     name,
                     name,
                     is_dir_entry ? "/" : "");

        if (n < 0 || send_all(client_fd, line, (size_t)n) == -1) {
            int saved = errno;
            closedir(dir);
            errno = saved;
            return -1;
        }
    }

    //HTML FOOTER
    const char *footer = "</ul></body></html>\n";
    if (send_all(client_fd, footer, strlen(footer)) == -1) {
        int saved = errno;
        closedir(dir);
        errno = saved;
        return -1;
    }

    closedir(dir);
    return 0;
}

