#define FUSE_USE_VERSION 35
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#include <fuse.h>

#include "filesystem.hpp"
#include "mtp_device.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mtp {
namespace {

using Clock = std::chrono::steady_clock;
using TempFile = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
constexpr unsigned block_size = 4096;

[[noreturn]] void fail(int error)
{
    throw std::system_error(error, std::generic_category());
}

template<typename Operation>
int reply(const char* action, Operation operation) noexcept
{
    try {
        return operation();
    } catch (const std::system_error& error) {
        if (!error.code().value() || error.code().value() == EIO ||
            error.code().value() == ENODEV || error.code().value() == ETIMEDOUT ||
            std::strcmp(action, "upload") == 0) {
            std::fprintf(stderr, "mtp_drive: %s: %s\n", action, error.what());
        }
        return error.code().value() > 0 ? -error.code().value() : -EIO;
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    } catch (...) {
        std::fprintf(stderr, "mtp_drive: %s failed\n", action);
        return -EIO;
    }
}

template<typename Operation>
void recover(const char* action, Operation operation) noexcept
{
    try {
        operation();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "mtp_drive: %s: %s\n", action, error.what());
    }
}

struct Folder {
    uint32_t storage;
    uint32_t id;

    bool operator<(const Folder& other) const
    {
        return std::tie(storage, id) < std::tie(other.storage, other.id);
    }

    bool operator==(const Folder& other) const
    {
        return storage == other.storage && id == other.id;
    }
};

struct Location {
    Folder parent;
    std::string name;

    bool operator<(const Location& other) const
    {
        return std::tie(parent, name) < std::tie(other.parent, other.name);
    }
};

struct OpenFile {
    Location location;
    std::optional<Object> object;
    TempFile data{std::tmpfile(), std::fclose};
    uint64_t size = 0;
    std::time_t modified = std::time(nullptr);
    unsigned references = 0;
    int staging_error = 0;
    bool dirty = false;
    bool flush_attempted = false;

    int fd() const { return fileno(data.get()); }
};

struct Entry {
    const Storage* storage = nullptr;
    std::optional<Object> object;
    OpenFile* file = nullptr;

    bool directory() const { return !file && (!object || object->directory); }

    Folder folder() const
    {
        if (!directory())
            fail(ENOTDIR);
        return {storage->id, object ? object->id : root};
    }
};

using Directory = std::map<std::string, Object>;

struct CachedDirectory {
    Directory entries;
    Clock::time_point expires;
};

bool valid_name(std::string_view name)
{
    return !name.empty() && name != "." && name != ".." && name.size() <= NAME_MAX &&
           name.find('/') == std::string_view::npos;
}

std::optional<std::string> normalize(std::string_view name, CFStringNormalizationForm form)
{
    if (std::all_of(name.begin(), name.end(), [](unsigned char byte) { return byte < 128; }))
        return std::string(name);
    const std::unique_ptr<std::remove_pointer_t<CFStringRef>, decltype(&CFRelease)> input(
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(name.data()),
                                static_cast<CFIndex>(name.size()), kCFStringEncodingUTF8, false),
        CFRelease);
    if (!input)
        return std::nullopt;
    const std::unique_ptr<std::remove_pointer_t<CFMutableStringRef>, decltype(&CFRelease)> output(
        CFStringCreateMutableCopy(kCFAllocatorDefault, 0, input.get()), CFRelease);
    if (!output)
        fail(ENOMEM);
    CFStringNormalize(output.get(), form);
    const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(output.get()),
                                                       kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<size_t>(size), '\0');
    if (!CFStringGetCString(output.get(), result.data(), size, kCFStringEncodingUTF8))
        return std::nullopt;
    result.resize(std::strlen(result.c_str()));
    return result;
}

std::string path_name(std::string_view name)
{
    auto normalized = normalize(name, kCFStringNormalizationFormD);
    if (!normalized)
        fail(EINVAL);
    return std::move(*normalized);
}

std::string device_name(std::string_view name)
{
    auto normalized = normalize(name, kCFStringNormalizationFormC);
    if (!normalized)
        fail(EINVAL);
    if (!valid_name(*normalized))
        fail(normalized->size() > NAME_MAX ? ENAMETOOLONG : EINVAL);
    return std::move(*normalized);
}

// Reserve real names first so an alias never hides an object with that name.
template<typename Item, typename Id>
std::map<std::string, Item> visible_names(std::vector<Item> items, const char* prefix, Id id)
{
    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        return id(a) < id(b);
    });
    std::map<std::string, Item> result;
    std::vector<Item> aliases;
    for (auto& item : items) {
        // macFUSE passes decomposed names to callbacks and measures the limit in NFC bytes.
        const auto composed = normalize(item.name, kCFStringNormalizationFormC);
        if (composed && valid_name(*composed)) {
            auto name = path_name(*composed);
            if (!result.count(name)) {
                result.emplace(std::move(name), std::move(item));
                continue;
            }
        }
        aliases.push_back(std::move(item));
    }
    for (auto& item : aliases) {
        const std::string base = std::string(prefix) + std::to_string(id(item));
        std::string name = base;
        for (unsigned i = 2; result.count(name); ++i)
            name = base + " (" + std::to_string(i) + ')';
        result.emplace(std::move(name), std::move(item));
    }
    return result;
}

std::string temporary_name()
{
    uint32_t random[4];
    arc4random_buf(random, sizeof(random));
    std::ostringstream name;
    name << "mtp-drive-" << std::hex;
    for (const auto word : random)
        name << '-' << word;
    return name.str();
}

bool writing(const fuse_file_info* info)
{
    return (info->flags & O_ACCMODE) != O_RDONLY;
}

OpenFile& opened(const fuse_file_info* info)
{
    return *reinterpret_cast<OpenFile*>(static_cast<std::uintptr_t>(info->fh));
}

class Volume {
public:
    Volume(Device& device, bool read_only, bool kernel = false)
        : device_(device), read_only_(read_only), kernel_(kernel),
          storages_(visible_names(device.storages(), "Storage ", [](const Storage& storage) {
              return storage.id;
          }))
    {
        if (storages_.empty())
            fail(ENODEV);
    }

    void attributes(const char* path, struct stat& result)
    {
        const Entry entry = resolve(path);
        result = {};
        result.st_uid = getuid();
        result.st_gid = getgid();
        result.st_blksize = block_size;
        result.st_nlink = 1;
        const bool can_write = entry.storage && !read_only_ && entry.storage->writable;
        result.st_mode = entry.directory() ? S_IFDIR | (can_write ? 0755 : 0555)
                                          : S_IFREG | (can_write ? 0644 : 0444);
        const auto size = entry.file ? entry.file->size : entry.object ? entry.object->size : 0;
        if (!entry.directory()) {
            if (size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
                fail(EOVERFLOW);
            result.st_size = static_cast<off_t>(size);
            result.st_blocks = static_cast<blkcnt_t>(size / 512 + (size % 512 != 0));
        }
        const auto modified = entry.file ? entry.file->modified :
                              entry.object ? entry.object->modified : 0;
        result.st_atime = result.st_mtime = result.st_ctime = modified ? modified : mounted_;
    }

    void readdir(const char* path, void* buffer, fuse_fill_dir_t fill, off_t offset)
    {
        const auto entry = resolve(path);
        std::vector<std::string> names{".", ".."};
        if (entry.storage) {
            const auto children = names_in(entry.folder());
            names.insert(names.end(), children.begin(), children.end());
        } else {
            for (const auto& [name, storage] : storages_)
                names.push_back(name);
        }
        if (offset < 0)
            fail(EINVAL);
        for (auto i = static_cast<size_t>(offset); i < names.size(); ++i) {
            if (fill(buffer, names[i].c_str(), nullptr, static_cast<off_t>(i + 1),
                     fuse_fill_dir_flags{}))
                break;
        }
    }

    void statfs(const char* path, struct statvfs& result)
    {
        const auto entry = resolve(path);
        if (Clock::now() >= stats_expire_) {
            capacity_ = device_.storages();
            stats_expire_ = Clock::now() + std::chrono::seconds(5);
        }
        result = {};
        result.f_bsize = result.f_frsize = block_size;
        result.f_namemax = NAME_MAX;
        bool can_write = false;
        for (const auto& storage : capacity_) {
            if (entry.storage && entry.storage->id != storage.id)
                continue;
            result.f_blocks += storage.capacity / block_size;
            result.f_bfree += storage.free_bytes / block_size;
            if (!read_only_ && storage.writable) {
                can_write = true;
                result.f_bavail += storage.free_bytes / block_size;
            }
        }
        if (!can_write)
            result.f_flag = ST_RDONLY;
    }

    void open(const char* path, fuse_file_info* info, bool create)
    {
        Location location = locate(path, create ? EROFS : EISDIR);
        const auto entry = find(location);
        if (create || writing(info))
            ensure_writable(location.parent);
        if (create && entry)
            fail(EEXIST);
        if (!create && !entry)
            fail(ENOENT);
        if (entry && entry->directory())
            fail(EISDIR);

        const bool truncate = writing(info) && (info->flags & O_TRUNC);
        auto& file = entry && entry->file ?
            *entry->file : stage(location, entry ? entry->object : std::nullopt, !truncate);
        if (truncate) {
            try {
                resize(file, 0);
            } catch (...) {
                if (!file.references)
                    files_.erase(location);
                throw;
            }
        }
        ++file.references;
        info->fh = reinterpret_cast<std::uintptr_t>(&file);
    }

    int read(char* buffer, size_t size, off_t offset, const fuse_file_info* info)
    {
        const auto count = pread(opened(info).fd(), buffer, std::min(size, size_t{INT_MAX}), offset);
        if (count < 0)
            fail(errno);
        return static_cast<int>(count);
    }

    int write(const char* buffer, size_t size, off_t offset, const fuse_file_info* info)
    {
        auto& file = opened(info);
        ensure_writable(file.location.parent);
        if (file.staging_error)
            fail(file.staging_error);
        if (size > INT_MAX)
            fail(EINVAL);
        if (info->flags & O_APPEND)
            offset = static_cast<off_t>(file.size);
        const auto count = pwrite(file.fd(), buffer, size, offset);
        if (count < 0 || static_cast<size_t>(count) != size) {
            file.staging_error = count < 0 ? errno : ENOSPC;
            file.flush_attempted = false;
            fail(file.staging_error);
        }
        if (count) {
            file.size = std::max(file.size, static_cast<uint64_t>(offset) + count);
            changed(file);
        }
        return static_cast<int>(count);
    }

    void truncate(const char* path, off_t size, fuse_file_info* info)
    {
        if (info && info->fh) {
            ensure_writable(opened(info).location.parent);
            resize(opened(info), size);
            return;
        }
        const auto location = locate(path, EISDIR);
        ensure_writable(location.parent);
        const auto entry = find(location);
        if (!entry)
            fail(ENOENT);
        if (entry->directory())
            fail(EISDIR);
        if (entry->file) {
            resize(*entry->file, size);
            commit(*entry->file);
            return;
        }
        auto& file = stage(location, entry->object, size != 0);
        try {
            resize(file, size);
            commit(file);
        } catch (...) {
            files_.erase(location);
            throw;
        }
        files_.erase(location);
    }

    void flush(const fuse_file_info* info)
    {
        commit(opened(info));
    }

    int release(const fuse_file_info* info)
    {
        auto& file = opened(info);
        const int result = reply("upload", [&] {
            if (!file.flush_attempted)
                commit(file);
            return 0;
        });
        if (--file.references == 0)
            files_.erase(files_.find(file.location));
        return result;
    }

    void mkdir(const char* path)
    {
        const auto location = locate(path, EROFS);
        ensure_writable(location.parent);
        if (find(location))
            fail(EEXIST);
        invalidate(location.parent);
        device_.mkdir(location.parent.storage, location.parent.id, device_name(location.name));
    }

    void remove(const char* path, bool directory)
    {
        const auto location = locate(path, EACCES);
        ensure_writable(location.parent);
        const auto entry = find(location);
        if (!entry)
            fail(ENOENT);
        if (entry->directory() != directory)
            fail(directory ? ENOTDIR : EISDIR);
        if (entry->file)
            fail(EBUSY);
        if (directory && !directory_empty(entry->folder()))
            fail(ENOTEMPTY);
        invalidate(location.parent);
        if (directory)
            directories_.erase(entry->folder());
        device_.erase(entry->object->id);
    }

    void rename(const char* source, const char* destination, unsigned flags)
    {
        if (flags & ~static_cast<unsigned>(RENAME_NOREPLACE))
            fail(EOPNOTSUPP);
        const auto from = locate(source, EACCES);
        const auto to = locate(destination, EACCES);
        if (from.parent.storage != to.parent.storage)
            fail(EXDEV);
        ensure_writable(from.parent);
        const auto entry = find(from);
        if (!entry)
            fail(ENOENT);
        if (from.parent == to.parent && from.name == to.name)
            return;
        const auto target = find(to);
        // FSKit forwards rename-swap requests as ordinary renames.
        if (target && (!kernel_ || (flags & RENAME_NOREPLACE)))
            fail(EEXIST);
        if (entry->file || (target && target->file))
            fail(EBUSY);
        if (target && target->directory() != entry->directory())
            fail(target->directory() ? EISDIR : ENOTDIR);
        if (target && target->directory() && !directory_empty(target->folder()))
            fail(ENOTEMPTY);
        const auto original_path = path_name(source);
        const auto new_path = path_name(destination);
        if (entry->directory() && new_path.size() > original_path.size() &&
            new_path.compare(0, original_path.size(), original_path) == 0 &&
            new_path[original_path.size()] == '/')
            fail(EINVAL);

        invalidate(from.parent);
        invalidate(to.parent);
        if (target && target->directory())
            directories_.erase(target->folder());
        replace(target ? target->object : std::nullopt, [&] {
            rename_object(*entry->object, from, to);
        });
    }

    void set_times(const char* path, const struct timespec times[2], fuse_file_info* info)
    {
        if (times && times[1].tv_nsec == UTIME_OMIT)
            return;
        auto* file = info && info->fh ? &opened(info) : resolve(path).file;
        if (!file || !file->dirty)
            fail(ENOTSUP);
        ensure_writable(file->location.parent);
        file->modified = !times || times[1].tv_nsec == UTIME_NOW ?
            std::time(nullptr) : times[1].tv_sec;
        file->flush_attempted = false;
    }

private:
    const Storage& storage(Folder folder) const
    {
        for (const auto& [name, storage] : storages_) {
            if (storage.id == folder.storage)
                return storage;
        }
        fail(ENODEV);
    }

    void ensure_writable(Folder folder) const
    {
        if (read_only_ || !storage(folder).writable)
            fail(EROFS);
    }

    const Directory& entries(Folder folder)
    {
        const auto now = Clock::now();
        const auto cached = directories_.find(folder);
        if (cached != directories_.end() && cached->second.expires > now)
            return cached->second.entries;
        if (directories_.size() >= 256)
            directories_.clear();
        auto objects = device_.children(folder.storage, folder.id);
        auto names = visible_names(std::move(objects), "Object ", [](const Object& object) {
            return object.id;
        });
        return directories_.insert_or_assign(folder, CachedDirectory{
            std::move(names), now + std::chrono::seconds(2)
        }).first->second.entries;
    }

    std::set<std::string> names_in(Folder folder)
    {
        std::set<std::string> names;
        for (const auto& [name, object] : entries(folder))
            names.insert(name);
        for (auto file = files_.lower_bound(Location{folder, {}});
             file != files_.end() && file->first.parent == folder; ++file)
            names.insert(file->first.name);
        return names;
    }

    bool directory_empty(Folder folder)
    {
        directories_.erase(folder);
        return names_in(folder).empty();
    }

    std::optional<Entry> find(const Location& location)
    {
        const auto& owner = storage(location.parent);
        const auto file = files_.find(location);
        if (file != files_.end())
            return Entry{&owner, {}, &file->second};
        const auto& directory = entries(location.parent);
        const auto item = directory.find(location.name);
        if (item == directory.end())
            return std::nullopt;
        return Entry{&owner, item->second, nullptr};
    }

    Entry resolve(std::string_view path)
    {
        if (path == "/")
            return {};
        path.remove_prefix(1);
        const auto separator = path.find('/');
        const auto volume = storages_.find(path_name(path.substr(0, separator)));
        if (volume == storages_.end())
            fail(ENOENT);
        Entry entry{&volume->second, {}, nullptr};
        auto slash = separator;
        while (slash != std::string_view::npos) {
            path.remove_prefix(slash + 1);
            slash = path.find('/');
            const auto item = find({entry.folder(), path_name(path.substr(0, slash))});
            if (!item)
                fail(ENOENT);
            entry = *item;
        }
        return entry;
    }

    Location locate(std::string_view path, int root_error)
    {
        const auto separator = path.rfind('/');
        if (separator == 0)
            fail(root_error);
        const auto parent = resolve(path.substr(0, separator)).folder();
        return {parent, path_name(device_name(path.substr(separator + 1)))};
    }

    OpenFile& stage(const Location& location, const std::optional<Object>& object, bool download)
    {
        OpenFile file{location, object};
        if (!file.data)
            fail(errno);
        if (object) {
            file.modified = object->modified;
            if (download) {
                device_.download(object->id, file.fd());
                struct stat stat{};
                if (fstat(file.fd(), &stat) != 0)
                    fail(errno);
                file.size = static_cast<uint64_t>(stat.st_size);
                if (file.size != object->size)
                    fail(EIO);
            }
        } else {
            file.dirty = true;
        }
        return files_.emplace(location, std::move(file)).first->second;
    }

    void changed(OpenFile& file)
    {
        file.modified = std::time(nullptr);
        file.dirty = true;
        file.flush_attempted = false;
    }

    void resize(OpenFile& file, off_t size)
    {
        if (ftruncate(file.fd(), size) != 0)
            fail(errno);
        if (size == 0)
            file.staging_error = 0;
        file.size = static_cast<uint64_t>(size);
        changed(file);
    }

    void invalidate(Folder folder)
    {
        directories_.erase(folder);
        stats_expire_ = {};
    }

    // MTP cannot overwrite objects atomically. Retain the original until publication succeeds.
    template<typename Publish>
    void replace(const std::optional<Object>& previous, Publish publish)
    {
        if (!previous) {
            publish();
            return;
        }
        const auto backup = temporary_name();
        device_.rename(previous->id, backup);
        try {
            publish();
        } catch (...) {
            recover(("restore " + backup).c_str(), [&] {
                device_.rename(previous->id, previous->name);
            });
            throw;
        }
        recover(("remove backup " + backup).c_str(), [&] { device_.erase(previous->id); });
    }

    void commit(OpenFile& file)
    {
        file.flush_attempted = true;
        if (file.staging_error)
            fail(file.staging_error);
        if (!file.dirty)
            return;
        const auto& [parent, name] = file.location;
        invalidate(parent);
        if (!file.object) {
            const auto& directory = entries(parent);
            if (directory.count(name))
                fail(EEXIST);
            invalidate(parent);
            auto filename = device_name(name);
            const auto id = device_.upload(parent.storage, parent.id, filename, file.fd(),
                                           file.size, file.modified);
            file.object = Object{id, std::move(filename), file.size, file.modified, false};
        } else {
            const auto staged = temporary_name();
            const auto id = device_.upload(parent.storage, parent.id, staged, file.fd(),
                                           file.size, file.modified);
            try {
                replace(file.object, [&] { device_.rename(id, file.object->name); });
            } catch (...) {
                recover(("remove staged file " + staged).c_str(), [&] { device_.erase(id); });
                throw;
            }
            file.object->id = id;
            file.object->size = file.size;
            file.object->modified = file.modified;
        }
        file.dirty = false;
    }

    void rename_object(const Object& object, const Location& from, const Location& to)
    {
        const auto filename = device_name(to.name);
        if (from.parent == to.parent) {
            device_.rename(object.id, filename);
            return;
        }
        if (object.name == filename) {
            device_.move(object.id, to.parent.storage, to.parent.id);
            return;
        }
        const auto temporary = temporary_name();
        device_.rename(object.id, temporary);
        try {
            device_.move(object.id, to.parent.storage, to.parent.id);
        } catch (...) {
            recover(("restore " + temporary).c_str(), [&] {
                device_.rename(object.id, object.name);
            });
            throw;
        }
        try {
            device_.rename(object.id, filename);
        } catch (...) {
            recover(("move back " + temporary).c_str(), [&] {
                device_.move(object.id, from.parent.storage, from.parent.id);
                device_.rename(object.id, object.name);
            });
            throw;
        }
    }

    Device& device_;
    bool read_only_;
    bool kernel_;
    const std::map<std::string, Storage> storages_;
    std::map<Folder, CachedDirectory> directories_;
    std::map<Location, OpenFile> files_;
    std::vector<Storage> capacity_;
    Clock::time_point stats_expire_{};
    const std::time_t mounted_ = std::time(nullptr);
};

Volume& volume()
{
    return *static_cast<Volume*>(fuse_get_context()->private_data);
}

fuse_operations operations()
{
    fuse_operations ops{};
    ops.init = [](fuse_conn_info* connection, fuse_config* config) -> void* {
        if (connection->capable & FUSE_CAP_ATOMIC_O_TRUNC)
            connection->want |= FUSE_CAP_ATOMIC_O_TRUNC;
        config->entry_timeout = 0;
        config->attr_timeout = 0;
        config->negative_timeout = 0;
        return &volume();
    };
    ops.getattr = [](const char* path, struct stat* stat, fuse_file_info*) {
        return reply("get attributes", [&] { volume().attributes(path, *stat); return 0; });
    };
    ops.readdir = [](const char* path, void* buffer, fuse_fill_dir_t fill, off_t offset,
                     fuse_file_info*, fuse_readdir_flags) {
        return reply("list directory", [&] { volume().readdir(path, buffer, fill, offset); return 0; });
    };
    ops.statfs = [](const char* path, struct statvfs* stat) {
        return reply("get capacity", [&] { volume().statfs(path, *stat); return 0; });
    };
    ops.open = [](const char* path, fuse_file_info* info) {
        return reply("open file", [&] { volume().open(path, info, false); return 0; });
    };
    ops.create = [](const char* path, mode_t, fuse_file_info* info) {
        return reply("create file", [&] { volume().open(path, info, true); return 0; });
    };
    ops.read = [](const char*, char* buffer, size_t size, off_t offset, fuse_file_info* info) {
        return reply("read file", [&] { return volume().read(buffer, size, offset, info); });
    };
    ops.write = [](const char*, const char* buffer, size_t size, off_t offset, fuse_file_info* info) {
        return reply("write file", [&] { return volume().write(buffer, size, offset, info); });
    };
    ops.truncate = [](const char* path, off_t size, fuse_file_info* info) {
        return reply("resize file", [&] { volume().truncate(path, size, info); return 0; });
    };
    ops.utimens = [](const char* path, const struct timespec times[2], fuse_file_info* info) {
        return reply("set times", [&] { volume().set_times(path, times, info); return 0; });
    };
    ops.flush = [](const char*, fuse_file_info* info) {
        return reply("upload", [&] { volume().flush(info); return 0; });
    };
    ops.fsync = [](const char*, int, fuse_file_info* info) {
        return reply("upload", [&] { volume().flush(info); return 0; });
    };
    ops.release = [](const char*, fuse_file_info* info) { return volume().release(info); };
    ops.mkdir = [](const char* path, mode_t) {
        return reply("create directory", [&] { volume().mkdir(path); return 0; });
    };
    ops.unlink = [](const char* path) {
        return reply("unlink file", [&] { volume().remove(path, false); return 0; });
    };
    ops.rmdir = [](const char* path) {
        return reply("remove directory", [&] { volume().remove(path, true); return 0; });
    };
    ops.rename = [](const char* source, const char* destination, unsigned flags) {
        return reply("rename", [&] { volume().rename(source, destination, flags); return 0; });
    };
    return ops;
}

} // namespace

int mount(Device& device, const std::string& mountpoint, bool read_only, bool kernel)
{
    Volume filesystem(device, read_only, kernel);
    auto name = device.name();
    for (auto& character : name) {
        if (character == ',' || character == '\\' ||
            static_cast<unsigned char>(character) < 32)
            character = ' ';
    }
    std::string options = "local,daemon_timeout=600,volname=" + name;
    if (!kernel)
        options += ",backend=fskit";
    if (read_only) {
        if (kernel) {
            options += ",ro";
        } else if (__builtin_available(macOS 26.4, *)) {
            options += ",ro";
        }
    }
    // One thread owns the USB session and the staged files.
    std::vector<std::string> arguments{"mtp_drive", "-f", "-s", "-o", options, mountpoint};
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto callbacks = operations();
    return fuse_main(static_cast<int>(arguments.size()), argv.data(), &callbacks, &filesystem);
}

} // namespace mtp
