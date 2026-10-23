#pragma once
#include "fsnet/storage.hpp"

namespace fsnet {
std::shared_ptr<Database> memory_database();
std::shared_ptr<Database> postgres_database(const std::string& connection_options);
std::shared_ptr<Database> sqlite_database(const std::string& filename);
}
