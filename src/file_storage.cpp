#include "fsnet/database.hpp"
#include "fsnet/entity_record.hpp"
#include <filesystem>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <cerrno>
#include <system_error>
#include <chrono>
#include <thread>
#ifdef __unix__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fsnet {
namespace {
constexpr std::size_t maximum_value = 16 * 1024 * 1024;
class Descriptor {
    int value_ = -1;
public:
    explicit Descriptor(int value) : value_(value) { if (value < 0) throw std::system_error(errno, std::generic_category(), "file storage open"); }
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
};
void check(int code, const char* what) { if (code < 0) throw std::system_error(errno, std::generic_category(), what); }
std::string digest(std::string_view data) {
    unsigned char result[32]; std::size_t size = sizeof result;
    if (!EVP_Q_digest(nullptr, "SHA256", nullptr, data.data(), data.size(), result, &size) || size != sizeof result)
        throw std::runtime_error("storage checksum failed");
    return std::string(reinterpret_cast<const char*>(result), sizeof result);
}
std::string hex(std::string_view input) {
    constexpr char digits[] = "0123456789abcdef"; std::string out;
    for (unsigned char byte : input) { out += digits[byte >> 4]; out += digits[byte & 15]; } return out;
}
void write_all(int descriptor, std::string_view data) {
    while (!data.empty()) {
        auto bytes = ::write(descriptor, data.data(), data.size());
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) throw std::system_error(bytes ? errno : EIO, std::generic_category(), "file storage write");
        data.remove_prefix(static_cast<std::size_t>(bytes));
    }
}
struct Entry { std::uint64_t version = 0; bool present = false; std::string value; };
class Files final : public Storage {
    Descriptor root_;
    static int root(const std::string& path) {
        if (path.empty() || path.find('\0') != std::string::npos) throw std::invalid_argument("file storage directory required");
        std::filesystem::create_directories(path);
        return ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    struct Locked {
        Descriptor directory, lock;
        std::string name;
        Locked(int directory_fd, const std::string& digest, bool exclusive)
          : directory(directory_fd), lock(::openat(directory.get(), (digest.substr(2) + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)), name(digest.substr(2) + ".record") {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                if (::flock(lock.get(), (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0) break;
                if (errno == EINTR) continue;
                if (errno != EWOULDBLOCK && errno != EAGAIN) check(-1, "file storage lock");
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("file storage lock deadline");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    };
    Locked lock(std::string_view key, bool exclusive) {
        if (key.empty() || key.size() > 4096) throw std::invalid_argument("invalid storage key");
        auto name = hex(digest(key)); auto shard = name.substr(0, 2);
        auto created = ::mkdirat(root_.get(), shard.c_str(), 0700);
        if (created < 0 && errno != EEXIST) check(-1, "file storage shard");
        if (!created || exclusive) check(::fsync(root_.get()), "file storage root sync");
        return Locked(::openat(root_.get(), shard.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW), name, exclusive);
    }
    Entry read(int directory,const std::string& name, std::string_view key) {
        auto descriptor = ::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0 && errno == ENOENT) return {};
        Descriptor file(descriptor); struct stat metadata{}; check(::fstat(file.get(), &metadata), "file storage stat");
        if (!S_ISREG(metadata.st_mode) || metadata.st_size < 53 || metadata.st_size > static_cast<off_t>(maximum_value + 8192))
            throw std::runtime_error("invalid file storage record size");
        std::string bytes(static_cast<std::size_t>(metadata.st_size), '\0'); std::size_t position = 0;
        while (position < bytes.size()) {
            auto count = ::read(file.get(), bytes.data() + position, bytes.size() - position);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("truncated file storage record");
            position += static_cast<std::size_t>(count);
        }
        std::string_view input(bytes); auto checksum = input.substr(input.size() - 32); input.remove_suffix(32);
        if (digest(input) != checksum || !input.starts_with("FSF1")) throw std::runtime_error("file storage integrity check failed");
        input.remove_prefix(4); Entry result; result.version = entity_record_detail::integer(input);
        if (!result.version || result.version >= static_cast<std::uint64_t>(INT64_MAX) || input.empty() || (input[0] != 0 && input[0] != 1))
            throw std::runtime_error("invalid file storage revision");
        result.present = input[0] == 1; input.remove_prefix(1);
        if (snapshot_detail::blob(input) != key) throw std::runtime_error("file storage key mismatch");
        result.value = snapshot_detail::blob(input);
        if (!input.empty() || result.value.size() > maximum_value || (!result.present && !result.value.empty()))
            throw std::runtime_error("invalid file storage value");
        return result;
    }
    bool replace(std::string_view key, std::uint64_t expected, std::optional<std::string_view> value) {
        if (expected >= static_cast<std::uint64_t>(INT64_MAX) || (value && value->size() > maximum_value))
            throw std::invalid_argument("invalid storage replacement");
        auto locked = lock(key, true); auto current = read(locked.directory.get(),locked.name,key);
        if ((current.present ? current.version : 0) != expected || (!value && !current.present)) return false;
        if (current.version >= static_cast<std::uint64_t>(INT64_MAX) - 1) throw std::overflow_error("record version exhausted");
        std::string bytes = "FSF1"; entity_record_detail::integer(bytes, current.version + 1); bytes.push_back(value ? 1 : 0);
        snapshot_detail::blob(bytes, std::string(key)); snapshot_detail::blob(bytes, value ? std::string(*value) : std::string{});
        bytes += digest(bytes);
        std::array<unsigned char, 16> nonce{};
        if (RAND_bytes(nonce.data(), nonce.size()) != 1) throw std::runtime_error("storage temporary name generation failed");
        auto temporary = locked.name + '.' + hex(std::string_view(reinterpret_cast<const char*>(nonce.data()), nonce.size())) + ".tmp";
        try {
            Descriptor file(::openat(locked.directory.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
            write_all(file.get(), bytes); check(::fsync(file.get()), "file storage data sync");
            check(::renameat(locked.directory.get(), temporary.c_str(), locked.directory.get(), locked.name.c_str()), "file storage atomic replace");
            check(::fsync(locked.directory.get()), "file storage directory sync");
        } catch (...) { ::unlinkat(locked.directory.get(), temporary.c_str(), 0); throw; }
        return true;
    }
public:
    bool persistent() const noexcept override { return true; }
    explicit Files(const std::string& path) : root_(root(path)) {
        auto parent_path = std::filesystem::absolute(path).parent_path();
        Descriptor parent(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        check(::fsync(parent.get()), "file storage parent sync");
    }
    std::optional<Record> get(std::string_view key) override {
        if(key.empty()||key.size()>4096)throw std::invalid_argument("invalid storage key");
        auto name=hex(digest(key));auto shard=name.substr(0,2);
        auto fd=::openat(root_.get(),shard.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0&&errno==ENOENT)return std::nullopt;
        Descriptor directory(fd);
        // Writers only rename immutable files. An opened fd is a complete old
        // or new record; readers neither need nor create a per-key lock.
        auto record = read(directory.get(),name.substr(2)+".record",key);
        return record.present ? std::optional<Record>{{std::move(record.value), record.version}} : std::nullopt;
    }
    bool compare_exchange(std::string_view key, std::uint64_t expected, std::string_view value) override { return replace(key, expected, value); }
    bool erase(std::string_view key, std::uint64_t expected) override { return replace(key, expected, std::nullopt); }
};
}
std::shared_ptr<Storage> file_storage(const std::string& directory) { return std::make_shared<Files>(directory); }
}
#else
namespace fsnet { std::shared_ptr<Storage> file_storage(const std::string&) { throw std::runtime_error("file storage currently requires POSIX"); } }
#endif
