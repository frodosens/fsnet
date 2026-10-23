# Third-party sources

CMake downloads source archives over verified TLS and checks SHA256 before use. No third-party library binaries are committed. Fetched source trees retain upstream license and copyright notices.

| Component | Version | License | Purpose |
| --- | --- | --- | --- |
| standalone Asio | 1.30.2 | Boost Software License 1.0 | TCP async I/O |
| SQLiteCpp | 3.3.2 | MIT | SQLite C++ ownership/transactions |
| SQLite bundled in SQLiteCpp | see fetched `sqlite3/sqlite3.h` | Public domain | Embedded persistent storage |
| libjuice | 1.6.2 | MPL 2.0 | ICE/STUN/TURN client and relay |

Archive URLs and exact hashes are in the root CMakeLists.txt. The repository's historical `libs/` archives are separate legacy dependencies and are not part of the new build.
