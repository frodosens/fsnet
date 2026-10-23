# Third-party sources

CMake downloads source archives over verified TLS and checks SHA256 before use. No third-party library binaries are committed. Fetched source trees retain upstream license and copyright notices.

| Component | Version | License | Purpose |
| --- | --- | --- | --- |
| standalone Asio | 1.30.2 | Boost Software License 1.0 | TCP async I/O |
| SQLiteCpp | 3.3.2 | MIT | SQLite C++ ownership/transactions |
| SQLite bundled in SQLiteCpp | see fetched `sqlite3/sqlite3.h` | Public domain | Embedded persistent storage |
| Lua | 5.4.7 | MIT | Optional embedded script VM (built in C++ mode) |
| PostgreSQL / libpq | 17.11 | PostgreSQL License | Optional network storage adapter and private test server |
| OpenSSL | 3.5.9 (LTS) | Apache 2.0 | TLS 1.3, certificate identity and cryptography |
| libjuice | 1.6.2 | MPL 2.0 | ICE/STUN/TURN client and relay |

Archive URLs and exact hashes are in the root CMakeLists.txt and cmake/*.cmake. Historical dependency archives were removed from mainline and remain available in the `archive/legacy-c-ruby21` Git tag.
