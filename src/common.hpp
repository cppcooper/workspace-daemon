#pragma once
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
namespace wd {
using json = nlohmann::json;
namespace fs = std::filesystem;
constexpr size_t max_message = 65536;
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
inline void check(bool yes, const std::string &why) {
    if (!yes)
        throw Error(why);
}
inline void system_check(bool yes, const char *why) {
    if (!yes)
        throw Error(std::string(why) + ": " + strerror(errno));
}
struct FD {
    int value = -1;
    explicit FD(int n = -1) : value(n) {}
    ~FD() {
        if (value >= 0)
            close(value);
    }
    FD(const FD &) = delete;
    FD &operator=(const FD &) = delete;
    FD(FD &&other) noexcept : value(other.value) { other.value = -1; }
    int release() {
        int n = value;
        value = -1;
        return n;
    }
};
inline double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
inline std::string identifier(const json &v) {
    check(v.is_string(), "identifier must be a string");
    auto s = v.get<std::string>();
    check(!s.empty() && s.size() <= 96, "identifier length");
    for (unsigned char c : s)
        check((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-',
              "invalid identifier");
    return s;
}
inline void no_links(const fs::path &p) {
    check(p.is_absolute() && p.lexically_normal() == p, "absolute normalized path required");
    fs::path n;
    for (const auto &part : p) {
        n /= part;
        check(!fs::is_symlink(fs::symlink_status(n)), "symlink in control path");
    }
}
inline std::string read_text(const fs::path &p, size_t limit = max_message) {
    FD fd(open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    system_check(fd.value >= 0, "open control file");
    struct stat s{};
    system_check(fstat(fd.value, &s) == 0, "stat control file");
    check(S_ISREG(s.st_mode) && s.st_size >= 0 && static_cast<uint64_t>(s.st_size) <= limit,
          "invalid control file size/type");
    std::string text;
    char buf[4096];
    while (true) {
        auto n = read(fd.value, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        system_check(n >= 0, "read control file");
        if (!n)
            break;
        text.append(buf, static_cast<size_t>(n));
        check(text.size() <= limit, "control file too large");
    }
    return text;
}
inline void trusted(const fs::path &p) {
    no_links(p);
    for (auto parent = p.parent_path();; parent = parent.parent_path()) {
        struct stat directory_info{};
        system_check(lstat(parent.c_str(), &directory_info) == 0, "stat trusted parent");
        bool sticky_root = directory_info.st_uid == 0 && (directory_info.st_mode & S_ISVTX);
        check(S_ISDIR(directory_info.st_mode) &&
                  (directory_info.st_uid == getuid() || directory_info.st_uid == 0) &&
                  (!(directory_info.st_mode & 0022) || sticky_root),
              "unsafe trusted parent owner/mode");
        if (parent == parent.root_path())
            break;
    }
    struct stat s{};
    system_check(lstat(p.c_str(), &s) == 0, "stat trusted file");
    check(S_ISREG(s.st_mode) && (s.st_uid == getuid() || s.st_uid == 0) && !(s.st_mode & 0022),
          "unsafe trusted file owner/mode");
}
inline void write_all(int fd, const std::string &s) {
    size_t at = 0;
    while (at < s.size()) {
        auto n = write(fd, s.data() + at, s.size() - at);
        if (n < 0 && errno == EINTR)
            continue;
        system_check(n > 0, "write");
        at += static_cast<size_t>(n);
    }
}
inline std::string random_id();
inline void atomic_json(const fs::path &p, const json &value) {
    no_links(p);
    // A power loss may leave an unfinished temporary journal. Never let that
    // block a later restart, or treat it as a committed operation/catalog row.
    auto tmp = p.string() + ".pending-" + random_id();
    FD fd(open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    system_check(fd.value >= 0, "create journal");
    try {
        write_all(fd.value, value.dump() + "\n");
        system_check(fsync(fd.value) == 0, "fsync journal");
        fs::rename(tmp, p);
        FD dir(open(p.parent_path().c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC));
        system_check(dir.value >= 0 && fsync(dir.value) == 0, "fsync journal directory");
    } catch (...) {
        unlink(tmp.c_str());
        throw;
    }
}
inline void directory(const fs::path &p) {
    no_links(p);
    fs::create_directories(p);
    struct stat s{};
    system_check(stat(p.c_str(), &s) == 0, "stat directory");
    check(S_ISDIR(s.st_mode) && s.st_uid == getuid() && !(s.st_mode & 0022),
          "unsafe state directory");
}
inline std::string random_id() {
    std::ifstream f("/proc/sys/kernel/random/uuid");
    std::string s;
    std::getline(f, s);
    check(s.size() == 36, "random identity unavailable");
    return s;
}
inline sockaddr_un address(const std::string &path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    check(path.size() < sizeof a.sun_path, "socket path too long");
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return a;
}
inline json receive(int fd) {
    std::string line;
    char c;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (true) {
        check(std::chrono::steady_clock::now() < deadline, "socket message deadline");
        auto n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        system_check(n >= 0, "read socket");
        check(n != 0, "unterminated request");
        if (c == '\n')
            break;
        line += c;
        check(line.size() <= max_message, "request too large");
    }
    auto j = json::parse(line);
    check(j.is_object(), "JSON object required");
    return j;
}
inline json rpc(const std::string &socket, const json &request) {
    FD fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    system_check(fd.value >= 0, "socket");
    timeval tv{5, 0};
    setsockopt(fd.value, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd.value, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    auto a = address(socket);
    system_check(connect(fd.value, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0, "connect");
    write_all(fd.value, request.dump() + "\n");
    return receive(fd.value);
}
} // namespace wd
