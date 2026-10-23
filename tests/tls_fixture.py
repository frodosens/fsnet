"""Generate ephemeral test identities; never commit private keys."""
from pathlib import Path
import subprocess
import sys
import tempfile

openssl, executable = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='fsnet-tls-') as directory:
    root = Path(directory)
    from certificates import certificates
    certificates(openssl, root)
    subprocess.run([executable, directory], check=True)
