"""Crash after receiver state/inbox commits but before its ACK; reopen the store."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='fsnet-current-delivery-') as directory:
    uri = 'file:' + str(Path(directory) / 'records')
    crash = subprocess.run([sys.argv[1], '--crash', uri], capture_output=True, text=True, timeout=15)
    assert crash.returncode == 42, (crash.returncode, crash.stdout, crash.stderr)
    recovered = subprocess.run([sys.argv[1], '--recover', uri], capture_output=True, text=True, timeout=15, check=True)
    before = next(line for line in crash.stdout.splitlines() if line.startswith('entities '))
    after = next(line for line in recovered.stdout.splitlines() if line.startswith('entities '))
    assert before == after, (before, after)
    assert 'PASS current delivery' in recovered.stdout
    print(recovered.stdout, end='')
