"""Local plaintext fixture: echo first frame; no mTLS or role-check bypass claim."""

import argparse
import ctypes as C
from pathlib import Path
import select
import socket
import struct
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-dir", type=Path, default=Path(".dev/build/release"))
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
build = args.build_dir if args.build_dir.is_absolute() else repo / args.build_dir
library = C.CDLL(str(build / "libfsnet.so"))
library.fsnet_create.argtypes = [C.c_char_p]
library.fsnet_create.restype = C.c_void_p
library.fsnet_listen.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
library.fsnet_listen.restype = C.c_int
handler_type = C.CFUNCTYPE(None, C.c_uint64, C.c_void_p, C.c_size_t)
library.fsnet_bind.argtypes = [C.c_void_p, C.c_char_p, handler_type]
library.fsnet_poll.argtypes = [C.c_void_p]
library.fsnet_destroy.argtypes = [C.c_void_p]
library.fsnet_respond.argtypes = [C.c_void_p, C.c_uint64, C.c_int, C.c_char_p, C.c_size_t]
server = library.fsnet_create(b"server-config-only-fixture-secret")
called = []


@handler_type
def handle(request, data, length):
    called.append(C.string_at(data, length))
    library.fsnet_respond(server, request, 0, b"accepted", 8)


library.fsnet_bind(server, b"protected", handle)
port = library.fsnet_listen(server, b"127.0.0.1", 0)
peer = socket.create_connection(("127.0.0.1", port))
peer.setblocking(False)


def read_exact(size):
    output = b""
    end = time.monotonic() + 2
    while len(output) < size:
        if time.monotonic() >= end:
            raise TimeoutError("local read deadline")
        library.fsnet_poll(server)
        if select.select([peer], [], [], .001)[0]:
            chunk = peer.recv(size - len(output))
            if not chunk:
                raise EOFError("local peer closed")
            output += chunk
    return output


try:
    header = read_exact(4)
    body = read_exact(struct.unpack(">I", header)[0])
    # Client has no token argument: it echoes the server's received bytes.
    peer.sendall(header + body)
    route, data = b"protected", b"hello"
    request = struct.pack(">BBBBQH", 1, 1, 0, 0, 1, len(route)) + route + data
    peer.sendall(struct.pack(">I", len(request)) + request)
    reply_header = read_exact(4)
    reply = read_exact(struct.unpack(">I", reply_header)[0])
    print("raw_peer_had_no_preconfigured_token=True",
          "protected_handler_called=" + str(called == [data]),
          "response=" + repr(reply[14:]))
finally:
    peer.close()
    library.fsnet_destroy(server)
