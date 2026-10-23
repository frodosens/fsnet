"""Generate ephemeral test identities; never commit private keys."""
from pathlib import Path
import subprocess
import sys
import tempfile

openssl, executable, *ruby = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='fsnet-tls-') as directory:
    root = Path(directory)
    def run(*args):
        subprocess.run([openssl, *args], cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for ca in ('ca', 'untrusted'):
        run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', ca + '.key', '-out', ca + '.pem',
            '-subj', '/CN=' + ca, '-days', '1', '-addext', 'basicConstraints=critical,CA:TRUE')
    for name in ('server', 'client', 'rogue'):
        run('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', name + '.key', '-out', name + '.csr', '-subj', '/CN=' + name + '.fsnet')
        (root / 'extensions').write_text('subjectAltName=DNS:' + name + '.fsnet,IP:127.0.0.1\nextendedKeyUsage=serverAuth,clientAuth\nbasicConstraints=critical,CA:FALSE\n')
        ca = 'untrusted' if name == 'rogue' else 'ca'
        run('x509', '-req', '-in', name + '.csr', '-CA', ca + '.pem', '-CAkey', ca + '.key', '-CAcreateserial',
            '-out', name + '.pem', '-days', '1', '-extfile', 'extensions')
    subprocess.run([executable, directory], check=True)
    if ruby:
        subprocess.run([ruby[0], ruby[1], directory], check=True)
