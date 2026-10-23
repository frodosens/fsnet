#include "fsnet/database.hpp"

namespace fsnet {
std::shared_ptr<Storage> open_storage(const std::string& uri) {
    if (uri.empty() || uri == "memory:") return memory_database();
    if (uri.starts_with("file:")) return file_storage(uri.substr(5));
    if (uri.starts_with("sqlite:")) return sqlite_database(uri.substr(7));
    if (uri.starts_with("postgresql://") || uri.starts_with("postgres://")) return postgres_database(uri);
    throw std::invalid_argument("unsupported storage URI");
}
}
