#pragma once

#include <cstdint>
#include <ctime>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mtp {

inline constexpr uint32_t root = UINT32_MAX;

struct Storage {
    uint32_t id;
    std::string name;
    uint64_t capacity;
    uint64_t free_bytes;
    bool writable;
};

struct Object {
    uint32_t id;
    std::string name;
    uint64_t size;
    std::time_t modified;
    bool directory;
};

class Device {
public:
    static void list(std::ostream& output);
    explicit Device(std::optional<std::string> selector);
    ~Device();

    std::string name() const;
    std::vector<Storage> storages();
    std::vector<Object> children(uint32_t storage, uint32_t parent);
    void download(uint32_t id, int fd);
    uint32_t upload(uint32_t storage, uint32_t parent, const std::string& name,
                    int fd, uint64_t size, std::time_t modified);
    uint32_t mkdir(uint32_t storage, uint32_t parent, const std::string& name);
    void erase(uint32_t id);
    void rename(uint32_t id, const std::string& name);
    void move(uint32_t id, uint32_t storage, uint32_t parent);

private:
    struct Session;
    std::unique_ptr<Session> session_;
};

} // namespace mtp
