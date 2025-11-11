import os, socket, time, tempfile, shutil, subprocess, contextlib, threading
from pathlib import Path
from http.client import HTTPConnection, HTTPResponse
from typing import Iterator, Tuple, Optional
import unittest

from . import config

def _find_free_port() -> int:
    if config.MYHTTP_PORT:
        return config.MYHTTP_PORT
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

@contextlib.contextmanager
def temp_docroot(files: dict = None) -> Iterator[Path]:
    d = Path(tempfile.mkdtemp(prefix="myhttp-docroot-"))
    try:
        if files:
            for rel, content in files.items():
                p = d / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                if isinstance(content, bytes):
                    p.write_bytes(content)
                else:
                    p.write_text(content, encoding="utf-8")
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)

def wait_for_port(host: str, port: int, timeout: float) -> None:
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return
        except OSError as e:
            last_err = e
            time.sleep(0.05)
    raise TimeoutError(f"Server did not open {host}:{port} in time: {last_err}")


@contextlib.contextmanager
def start_server(docroot: Path, port: Optional[int] = None, extra_args: Optional[list] = None):
    if not config.MYHTTP_BIN.exists():
        raise unittest.SkipTest(f"Server binary not found: {config.MYHTTP_BIN}")

    port = port or _find_free_port()
    args = [str(config.MYHTTP_BIN), "-p", str(port), "-d", str(docroot)]
    if extra_args:
        args += extra_args

    # Open pipes only if you want to read them; otherwise see Option 2
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=os.environ.copy())
    try:
        wait_for_port("127.0.0.1", port, timeout=config.STARTUP_TIMEOUT)
        yield proc, ("127.0.0.1", port)
    finally:
        # Terminate/kill and then drain & close pipes
        try:
            proc.terminate()
            proc.wait(timeout=2.0)
        except Exception:
            proc.kill()
            try:
                proc.wait(timeout=1.0)
            except Exception:
                pass

        # Drain (optional), then close both streams to avoid ResourceWarning
        out_text = err_text = ""
        try:
            if proc.stdout is not None:
                try:
                    out_text = proc.stdout.read() or ""
                finally:
                    proc.stdout.close()
            if proc.stderr is not None:
                try:
                    err_text = proc.stderr.read() or ""
                finally:
                    proc.stderr.close()
        except Exception:
            # Ignore read errors on shutdown
            pass

        if config.VERBOSE:
            if out_text.strip():
                print("[server stdout]\n", out_text)
            if err_text.strip():
                print("[server stderr]\n", err_text)

def http_get(host: str, port: int, path: str, headers: dict = None, timeout: float = None) -> Tuple[int, dict, bytes]:
    timeout = timeout or config.REQ_TIMEOUT
    conn = HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request("GET", path, headers=headers or {})
        resp: HTTPResponse = conn.getresponse()
        body = resp.read()
        return resp.status, dict(resp.getheaders()), body
    finally:
        conn.close()

def http_request(host: str, port: int, method: str, path: str, body: bytes | str | None = None, headers: dict | None = None, timeout: float | None = None):
    """Generic HTTP request using http.client. Returns (status, headers_dict, body_bytes)."""
    timeout = timeout or config.REQ_TIMEOUT
    if isinstance(body, str):
        body = body.encode('utf-8')
    conn = HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request(method.upper(), path, body=body, headers=headers or {})
        resp: HTTPResponse = conn.getresponse()
        data = resp.read()
        return resp.status, dict(resp.getheaders()), data
    finally:
        conn.close()


def is_unsupported_method(status: int) -> bool:
    """Check for HTTP status codes that mean the method isn't implemented."""
    return status in (405, 501)

def http_get_raw(host: str, port: int, raw_request: bytes, recv_size: int = 65536) -> bytes:
    # Used for tightly-controlled parser blackbox tests
    with socket.create_connection((host, port), timeout=config.REQ_TIMEOUT) as s:
        s.sendall(raw_request)
        s.shutdown(socket.SHUT_WR)
        return s.recv(recv_size)

class RequiresServerBinary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not config.MYHTTP_BIN.exists():
            raise unittest.SkipTest(f"Server binary not found: {config.MYHTTP_BIN}")
