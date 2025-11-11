import unittest, threading, time, os
from pathlib import Path
from .utils import start_server, temp_docroot, http_get, RequiresServerBinary

class TestFSRace(RequiresServerBinary):
    def test_hot_reload_read_consistency(self):
        # This simulates a file being modified while multiple readers fetch it.
        # Even if your server doesn't guarantee atomic read of changing files,
        # this test can catch crashes or partial reads due to races.
        with temp_docroot({ "hot.txt": "v0" }) as docroot:
            target = Path(docroot) / "hot.txt"

            with start_server(Path(docroot)) as (proc, addr):
                stop = False
                errors = []

                def editor():
                    i = 1
                    while not stop:
                        tmp = target.with_suffix(".tmp")
                        content = f"v{i}"
                        tmp.write_text(content, encoding="utf-8")
                        os.replace(tmp, target)  # atomic rename on POSIX
                        i += 1

                def reader():
                    try:
                        status, headers, body = http_get(*addr, "/hot.txt")
                        if status != 200:
                            errors.append(("status", status))
                        # Accept any full version "vN", reject empty/partial
                        txt = body.decode(errors='ignore').strip()
                        if not txt.startswith("v"):
                            errors.append(("partial", txt))
                    except Exception as e:
                        errors.append(("exc", str(e)))

                th_edit = threading.Thread(target=editor, daemon=True)
                th_edit.start()

                readers = [threading.Thread(target=reader) for _ in range(100)]
                for t in readers: t.start()
                for t in readers: t.join()

                stop = True
                th_edit.join(timeout=0.5)
                self.assertFalse(errors, f"Inconsistent reads or errors: {errors[:5]}...")
