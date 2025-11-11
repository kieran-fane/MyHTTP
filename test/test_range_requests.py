import unittest
from .utils import start_server, temp_docroot, http_get, RequiresServerBinary
from pathlib import Path

class TestRangeRequests(RequiresServerBinary):
    def test_basic_range(self):
        data = "ABCDEFGHIJKLMNOPQRSTUVWXYZ" * 1000
        with temp_docroot({ "big.txt": data }) as docroot:
            with start_server(Path(docroot)) as (proc, addr):
                status, headers, body = http_get(*addr, "/big.txt", headers={"Range": "bytes=0-9"})
                if status == 206:
                    self.assertEqual(body.decode(), data[:10])
                else:
                    # If range unsupported, ensure we still get the full file 200
                    self.assertEqual(status, 200)
                    self.assertEqual(body.decode(), data)
