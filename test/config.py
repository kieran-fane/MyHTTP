import os
from pathlib import Path

# Path to your compiled server binary (can override with env)
MYHTTP_BIN = Path(os.environ.get("MYHTTP_BIN", "./MyHTTP")).resolve()

# Optional: fixed port for debugging. Otherwise a free port is chosen for each test module.
MYHTTP_PORT = int(os.environ.get("MYHTTP_PORT", "0"))  # 0 means auto

# Default startup timeout (seconds)
STARTUP_TIMEOUT = float(os.environ.get("MYHTTP_STARTUP_TIMEOUT", "5.0"))

# Per-request timeout (seconds)
REQ_TIMEOUT = float(os.environ.get("MYHTTP_REQ_TIMEOUT", "3.0"))

# Enable verbose prints from utils if needed
VERBOSE = os.environ.get("MYHTTP_TEST_VERBOSE", "0") == "1"
