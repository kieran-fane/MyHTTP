# MyHTTP – Lightweight C HTTP File Server

A fast, minimalist HTTP/1.1 file server written entirely in C.
It safely joins request paths within a specified document root, prevents directory traversal attacks, and supports concurrent clients using a multithreaded work queue.

---

## Features

- **Safe Path Resolution** — Uses `fs_join_safe()` to prevent traversal or symlink escapes.
- **Static File Serving** — Supports HTML, CSS, JS, images, and other MIME types via `fs_mime_from_path()`.
- **Directory Listing** — Automatically generates an HTML index when no `index.html` is present.
- **Thread Pool** — Handles multiple clients concurrently via a bounded MPMC work queue (`workq.c`).
- **Persistent Connections** — HTTP/1.1 keep-alive by default (closes properly when requested).
- **Robust Error Handling** — Proper 400/403/404/405 responses for invalid or forbidden requests.

---

## Design Overview

```
+-----------------------+
|       main.c          |
|  - Socket setup       |
|  - Worker threads     |
|  - HTTP dispatch      |
+----------+------------+
           |
           v
+-----------------------+
|       fs.c            |
|  - Safe path joining  |
|  - MIME detection     |
|  - Index lookup       |
|  - Directory listing  |
+----------+------------+
           |
           v
+-----------------------+
|     workq.c/.h        |
|  - Bounded ring buffer|
|  - Thread synchronization |
+----------+------------+
           |
           v
+-----------------------+
|    http_parse.c/.h    |
|  - Request parsing    |
|  - Header extraction  |
+-----------------------+
```

---

## Usage

### Build

```bash
gcc -std=c11 -O2 -Wall -Wextra -Werror=pedantic   -o myhttp main.c http_parse.c fs.c workq.c -lpthread
```

### Run

```bash
./myhttp -p 8080 -d /path/to/docroot
```

By default:
- Port: **8080**
- Document root: **current directory**

### Test

```bash
curl -v http://127.0.0.1:8080/
curl -v http://127.0.0.1:8080/some/file.txt
```

---

## Example Output

```bash
[*] Worker handling 127.0.0.1:55022 (fd=4)
GET /index.html -> 200 OK
GET /doesnotexist -> 404 Not Found
GET /dir/ -> Directory Listing
```

---

## Configuration Flags

| Flag | Description | Default |
|------|--------------|----------|
| `-p <port>` | Port to listen on | `8080` |
| `-d <dir>` | Document root directory | `.` |

---

## Known Limitations

- No HTTPS (TLS) support yet.
- Limited to basic static file serving.
- No range requests or caching headers.
- Only tested on Linux and macOS.
---

## TODO

- [ ] **Implement write methods (POST, PUT, PATCH, DELETE)** — Currently returns `501 Not Implemented`.
- [ ] Add **logging configuration** and proper access logs.
- [ ] Implement **MIME type configuration file** for extensibility.
- [ ] Add **unit tests** for `fs_join_safe()` and `workq`.
- [ ] Add **graceful shutdown** with signal handling.
- [ ] Optional **SSL/TLS layer** (via OpenSSL).

---
Developing as a learning project to understand HTTP servers and filesystem safety in C.
