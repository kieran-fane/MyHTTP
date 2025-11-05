#ifndef MYHTTP_FS_H
#define MYHTTP_FS_H

#include <stddef.h>  // size_t
#include <stdbool.h>

/* Safely join docroot (already realpath-resolved) with a decoded request path.
   Ensures the result stays within docroot (no traversal/symlink escape).
   'out' receives an absolute path (NUL-terminated) on success.
   Returns 0 on success, -1 on error. */
int  fs_join_safe(const char *docroot_real, const char *decoded_req_path,
                  char *out, size_t outlen);

/* Returns 1 if 'abs_path' is a directory, 0 if not, -1 on error. */
int  fs_is_dir(const char *abs_path);

/* If 'dir_abs' contains 'index_name', write its absolute path to 'out'.
   Returns 1 if found, 0 if not found, -1 on error. */
int  fs_try_index(const char *dir_abs, const char *index_name,
                  char *out, size_t outlen);

/* Open file read-only; returns fd >= 0 on success, -1 on error. */
int  fs_open_ro(const char *abs_path);

/* Simple MIME-by-extension helper (returns const string; never NULL).
   Example: ".html" -> "text/html"; unknown -> "application/octet-stream". */
const char* fs_mime_from_path(const char *abs_path);

/* Stream a minimal HTML directory listing to 'client_fd'.
   'req_path_display' is the URL path to show in links.
   Returns 0 on success, -1 on error. */
int  fs_send_dir_listing(int client_fd, const char *dir_abs,
                         const char *req_path_display);

/* Atomically replace/create the file addressed by (docroot_real + decoded_req_path)
   by streaming exactly content_len bytes from client_fd into a same-dir temp file,
   fsync() it, then rename() into place. Returns:
      1  -> file created
      0  -> file replaced
     -1  -> error (errno set) */
int fs_put_from_socket_atomic(const char *docroot_real,
                              const char *decoded_req_path,
                              int client_fd, size_t content_len);

/* Append exactly content_len bytes from client_fd to the target file.
   Creates the file if it doesn't exist. Returns 0 on success, -1 on error. */
int fs_append_from_socket(const char *docroot_real,
                          const char *decoded_req_path,
                          int client_fd, size_t content_len);

/* Safely unlink a regular file (not directories). Returns 0 on success, -1 on error. */
int fs_unlink_safe(const char *docroot_real, const char *decoded_req_path);

#endif /* MYHTTPD_FS_H */

