#pragma once
#include "fsnet/storage.hpp"

namespace fsnet {
std::shared_ptr<Storage> memory_database();
std::shared_ptr<Storage> postgres_database(const std::string& connection_options);
std::shared_ptr<Storage> sqlite_database(const std::string& filename);
}
