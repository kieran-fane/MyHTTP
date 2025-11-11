# MyHTTP Test Suite (Python, stdlib)

This test suite is designed for **black-box testing** of your C web server without adding any Python dependencies.
It uses Python's built-in `unittest`, `http.client`, `socket`, `subprocess`, and `threading` modules.

## Layout

```
test/
  README.md
  run_tests.py
  config.py
  utils.py
  __init__.py
  test_server_basic.py
  test_concurrency.py
  test_fs_race.py
  test_range_requests.py
```

> You can add more files like `test_http_parse_blackbox.py` or `test_*` modules to keep tests sectional.

## How it works

- Tests **spawn your server** as a subprocess so we can hit it over TCP like real clients.
- We set up a **temporary docroot** with sample files for each test to avoid touching your repo files.
- We choose a **random free port** per test module to avoid collisions when running tests in parallel.
- If the server binary isn't found, relevant tests will **skip** with a clear message.

## Assumptions

- Your built server binary is available at `./MyHTTP` by default. (Project root)
  - Override via environment variable: `MYHTTP_BIN=./path/to/binary`
- Your server accepts flags: `-p <port>` and `-d <docroot>` (adjust in `utils.py` if different).
- Your server supports basic `GET` for static files and returns sane status lines and headers.
- Range requests and concurrency safety are optional; tests will skip gracefully if not supported.

## Usage

From the project root (same directory as `build/` and `src/`):

```bash
# (Optional) build your server first
make -j

# Run all tests
python3 -m unittest discover -s test -t . -v

# Or, run a particular file
python3 -m unittest test.test_server_basic -v
python3 -m unittest test.test_concurrency -v
python3 -m unittest test.test_fs_race -v
python3 -m unittest test.test_range_requests -v
```

## Makefile target (optional)

Add this snippet to your project `Makefile` to wire in a `make test` target:

```make
.PHONY: test
test: all
	@echo "Running Python tests..."
	python3 -m unittest discover -s test -t . -v
```

## Extending

- Create new files like `test/test_http_parse_blackbox.py` to send crafted HTTP requests
  and validate parsing behavior via server responses.
- To test **path locking** and race conditions, see `test_fs_race.py` and `test_concurrency.py`
  for patterns using threads and many simultaneous connections.

## Troubleshooting

- **Binary not found / wrong args**: Update `MYHTTP_BIN` or tweak `utils.start_server()`.
- **Port in use**: Utils pick a free port, but if your server binds differently, set `MYHTTP_PORT`.
- **Hanging / no response**: Check your server logs; enable logging to stderr to see issues.
