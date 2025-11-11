import unittest, threading, time
from pathlib import Path
from .utils import start_server, temp_docroot, http_get, RequiresServerBinary

class TestConcurrency(RequiresServerBinary):
    def test_many_parallel_gets(self):
        with temp_docroot({ f"f{i}.txt": f"file {i}" for i in range(100) }) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                errs = []
                def worker(i):
                    try:
                        status, headers, body = http_get(*addr, f"/f{i%100}.txt")
                        if status != 200 or (body.decode() != f"file {i%100}"):
                            errs.append((i, status, body[:40]))
                    except Exception as e:
                        errs.append((i, str(e)))

                threads = [threading.Thread(target=worker, args=(i,)) for i in range(200)]
                for t in threads: t.start()
                for t in threads: t.join()
                self.assertFalse(errs, f"Errors in parallel GETs: {errs[:5]}... (total {len(errs)})")
