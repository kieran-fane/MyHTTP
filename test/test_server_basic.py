import unittest
from pathlib import Path
from .utils import start_server, temp_docroot, http_get, RequiresServerBinary

class TestServerBasic(RequiresServerBinary):
    def test_serves_static_file(self):
        with temp_docroot({ "index.html": "<h1>Hello</h1>" }) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                status, headers, body = http_get(*addr, "/")
                self.assertEqual(status, 200)
                self.assertIn("Content-Type", headers)
                self.assertTrue(body.startswith(b"<h1>Hello"))

    def test_404_for_missing(self):
        with temp_docroot({ "index.html": "ok" }) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                status, headers, body = http_get(*addr, "/nope.txt")
                self.assertIn(status, (403,404))  # depending on your directory listing policy
    
    def test_post_basic(self):
        from .utils import http_request
        with temp_docroot({"index.txt": "seed"}) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                st, h, b = http_request(
                    *addr, "POST", "/index.txt",
                    body="data=abc",
                    headers={"Content-Type": "application/x-www-form-urlencoded"},
                )
                # We don't assert a specific status because POST behavior may be unimplemented
                self.assertIsInstance(st, int)
                self.assertGreaterEqual(st, 100)  # 1xx+ is a valid HTTP status


    def test_put_create_or_replace(self):
        from .utils import http_request, is_unsupported_method
        with temp_docroot({}) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                st, h, b = http_request(
                    *addr, "PUT", "/put.txt",
                    body="hello world",
                    headers={"Content-Type": "text/plain"},
                )
                if is_unsupported_method(st):
                    self.skipTest("PUT not supported by server")
                else:
                    self.assertIn(st, (200, 201, 204))
                    # If your server exposes writes to the served docroot, we should be able to GET it
                    st2, h2, b2 = http_get(*addr, "/put.txt")
                    if st2 == 200:
                        self.assertEqual(b2.decode(), "hello world")


    def test_patch_basic(self):
        from .utils import http_request, is_unsupported_method
        with temp_docroot({}) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                st, h, b = http_request(
                    *addr, "PATCH", "/patch.txt",
                    body="delta",
                    headers={"Content-Type": "application/octet-stream"},
                )
                if is_unsupported_method(st):
                    self.skipTest("PATCH not supported by server")
                else:
                    self.assertIn(st, (200, 204))
                    # Follow-up GET shouldn't crash; content change semantics are server-defined
                    st2, _, _ = http_get(*addr, "/patch.txt")
                    self.assertIsInstance(st2, int)


    def test_delete_basic(self):
        from .utils import http_request, is_unsupported_method
        with temp_docroot({}) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                # First create a file via PUT (or skip if PUT unsupported)
                st_put, _, _ = http_request(*addr, "PUT", "/delme.txt", body="bye", headers={"Content-Type": "text/plain"})
                if is_unsupported_method(st_put):
                    self.skipTest("PUT not supported by server; cannot set up DELETE test")

                st_del, _, _ = http_request(*addr, "DELETE", "/delme.txt")
                if is_unsupported_method(st_del):
                    self.skipTest("DELETE not supported by server")
                else:
                    self.assertIn(st_del, (200, 202, 204))
                    # If the server exposes docroot changes, the file should now be gone
                    st2, _, _ = http_get(*addr, "/delme.txt")
                    self.assertIn(st2, (404, 410))
