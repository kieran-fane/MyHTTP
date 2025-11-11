import unittest, threading, random
from pathlib import Path
from .utils import start_server, temp_docroot, http_get, http_request, is_unsupported_method, RequiresServerBinary


class TestConcurrencyWrites(RequiresServerBinary):
    def test_parallel_put_unique_files(self):
        """Spawn many PUTs in parallel to ensure no data races or corruption."""
        with temp_docroot({}) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                st_probe, _, _ = http_request(*addr, "PUT", "/probe.txt", body="x")
                if is_unsupported_method(st_probe):
                    self.skipTest("PUT not supported by server")
                N = 100
                errs = []

                def worker(i):
                    name = f"/p{i}.txt"
                    payload = f"val-{i}"
                    st, _, _ = http_request(*addr, "PUT", name, body=payload,
                                            headers={"Content-Type": "text/plain"})
                    if st not in (200, 201, 204):
                        errs.append((i, "put_status", st))

                threads = [threading.Thread(target=worker, args=(i,)) for i in range(N)]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()

                if errs:
                    self.fail(f"Errors during parallel PUTs (first 5): {errs[:5]}")
                for i in random.sample(range(N), k=min(10, N)):
                    st, _, body = http_get(*addr, f"/p{i}.txt")
                    if st == 200:
                        self.assertEqual(body.decode(), f"val-{i}")

    def test_parallel_delete(self):
        """Create files via PUT, then delete them concurrently."""
        with temp_docroot({}) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                st_probe, _, _ = http_request(*addr, "PUT", "/probe.txt", body="x")
                if is_unsupported_method(st_probe):
                    self.skipTest("PUT not supported by server (and DELETE depends on files)")

                N = 50
                for i in range(N):
                    http_request(*addr, "PUT", f"/d{i}.txt", body=f"v{i}")

                st_del_probe, _, _ = http_request(*addr, "DELETE", "/d0.txt")
                if is_unsupported_method(st_del_probe):
                    self.skipTest("DELETE not supported by server")

                errs = []

                def remover(i):
                    st, _, _ = http_request(*addr, "DELETE", f"/d{i}.txt")
                    if st not in (200, 202, 204, 404, 410):
                        errs.append((i, "del_status", st))

                threads = [threading.Thread(target=remover, args=(i,)) for i in range(N)]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()

                if errs:
                    self.fail(f"Errors during parallel DELETEs (first 5): {errs[:5]}")

    def test_mixed_writes(self):
        """Mix PUT, PATCH, and DELETE concurrently to ensure server stability."""
        with temp_docroot({}) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                st_probe_put, _, _ = http_request(*addr, "PUT", "/probe.txt", body="x")
                if is_unsupported_method(st_probe_put):
                    self.skipTest("PUT not supported by server (mixed write test skipped)")

                def op_put(i):
                    http_request(*addr, "PUT", f"/m{i}.txt", body=f"v{i}")

                def op_patch(i):
                    http_request(*addr, "PATCH", f"/m{i}.txt", body=f"p{i}")

                def op_delete(i):
                    http_request(*addr, "DELETE", f"/m{i}.txt")

                ops = [op_put, op_patch, op_delete]
                threads = []
                for i in range(60):
                    fn = random.choice(ops)
                    threads.append(threading.Thread(target=fn, args=(i,)))
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()

                # Ensure server still responds after the stress test
                st, _, _ = http_get(*addr, "/probe.txt")
                self.assertIsInstance(st, int)
