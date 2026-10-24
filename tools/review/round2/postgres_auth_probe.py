"""Local PostgreSQL protocol fixture, always using an explicit dummy credential."""
import argparse
import json
import os
import socket
import struct
import subprocess
import threading

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--executable', required=True)
args = parser.parse_args()
seen = {}


def full(connection, length):
    data = b''
    while len(data) < length:
        value = connection.recv(length - len(data))
        if not value:
            raise EOFError
        data += value
    return data


def message(kind, data):
    return kind + struct.pack('!I', 4 + len(data)) + data


with socket.socket() as listener:
    listener.bind(('127.0.0.2', 0))
    listener.listen()
    listener.settimeout(3)
    port = listener.getsockname()[1]

    def serve():
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                size = struct.unpack('!I', full(connection, 4))[0]
                startup = full(connection, size - 4)
                seen['startup_received_without_tls'] = startup[:4] == b'\x00\x03\x00\x00'
                connection.sendall(message(b'R', struct.pack('!I', 3)))
                seen['password_message'] = full(connection, 1) == b'p'
                size = struct.unpack('!I', full(connection, 4))[0]
                seen['dummy_password_received'] = full(connection, size - 4) == b'only-a-test-password\0'
                connection.sendall(message(b'R', struct.pack('!I', 0)) +
                                   message(b'S', b'server_version\0' + b'17.11\0') +
                                   message(b'S', b'client_encoding\0UTF8\0') + message(b'Z', b'I'))
                seen['read_write_probe'] = full(connection, 1) == b'Q'
                size = struct.unpack('!I', full(connection, 4))[0]
                full(connection, size - 4)
                field = b'transaction_read_only\0' + struct.pack('!IhIhih', 0, 0, 25, -1, -1, 0)
                connection.sendall(message(b'T', struct.pack('!h', 1) + field) +
                                   message(b'D', struct.pack('!hI', 1, 3) + b'off') +
                                   message(b'C', b'SHOW\0') + message(b'Z', b'I'))
                connection.recv(5)
        except (OSError, EOFError, struct.error) as error:
            seen['fixture_end'] = type(error).__name__

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    # Do not inherit a real service-file selector, host, or credential.
    environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
    environment.update(PGHOST='127.0.0.2', PGPORT=str(port), PGDATABASE='review',
                       PGUSER='review', PGPASSWORD='only-a-test-password')
    result = subprocess.run([args.executable], env=environment, capture_output=True, text=True, timeout=8)
    thread.join(timeout=4)
print(json.dumps({'observed': seen, 'returncode': result.returncode,
                  'stdout': result.stdout, 'stderr': result.stderr}))
