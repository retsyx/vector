// Runs shell commands on a Kindle through the Vector Shell scriptlet, over MTP.
//
// The Kindle's MTP view may not show files that change on the Kindle side, so
// every file the Kindle writes is a placeholder this tool uploaded first, which
// the Kindle overwrites in place. This tool also deletes every exchange file.
#include "mtp_device.hpp"

#include <fcntl.h>
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

const char* const usage_text =
    "Usage:\n"
    "  kshell [-t SECONDS] [-o BYTES] COMMAND...  run COMMAND on the Kindle (stdin script if omitted)\n"
    "  kshell pull KINDLE_PATH LOCAL_PATH         copy any file off the Kindle through Vector Shell\n"
    "  kshell get KINDLE_PATH LOCAL_PATH          download a file MTP lists in user storage (/mnt/us)\n"
    "  kshell put LOCAL_PATH KINDLE_DIR           upload a file into a user storage folder, replacing it\n"
    "  kshell ls [KINDLE_DIR]                     list a user storage folder as MTP sees it\n"
    "  kshell clean                               delete leftover exchange files\n"
    "  kshell stop                                ask Vector Shell to exit\n";

constexpr size_t rc_capacity = 64;

bool kindle(uint16_t vendor, uint16_t product, const char* model)
{
    if (vendor != 0x1949)
        return false;
    if (product == 0x9981)
        return true;
    return model && !strcasestr(model, "fire") &&
           (strcasestr(model, "kindle") || strcasestr(model, "paperwhite") ||
            strcasestr(model, "scribe") || strcasestr(model, "colorsoft") || strcasestr(model, "oasis"));
}

using Objects = std::vector<mtp::Object>;

std::optional<mtp::Object> lookup(const Objects& entries, const std::string& name)
{
    std::optional<mtp::Object> match;
    for (const auto& object : entries) {
        if (strcasecmp(object.name.c_str(), name.c_str()) != 0)
            continue;
        if (match)
            throw std::runtime_error("Ambiguous Kindle name: " + name);
        match = object;
    }
    return match;
}

std::string shell_quote(const std::string& text)
{
    std::string quoted = "'";
    for (char c : text)
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return quoted + "'";
}

struct TempFile {
    TempFile() : handle(std::tmpfile())
    {
        if (!handle)
            throw std::system_error(errno, std::generic_category(), "Create temporary file");
    }
    ~TempFile() { std::fclose(handle); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    int fd() const { return fileno(handle); }
    std::FILE* handle;
};

class Kindle {
public:
    Kindle() : device_(std::nullopt, kindle), storage_(user_storage()) {}

    uint32_t folder(uint32_t parent, const std::string& name)
    {
        if (auto entry = lookup(device_.children(storage_, parent), name)) {
            if (!entry->directory)
                throw std::runtime_error("Not a folder on the Kindle: " + name);
            return entry->id;
        }
        return device_.mkdir(storage_, parent, name);
    }

    // Resolves a /mnt/us path; a leading /mnt/us is optional.
    mtp::Object resolve(std::string path)
    {
        if (path.rfind("/mnt/us", 0) == 0)
            path.erase(0, 7);
        mtp::Object current{mtp::root, "", 0, 0, true};
        std::stringstream parts(path);
        for (std::string part; std::getline(parts, part, '/');) {
            if (part.empty() || part == ".")
                continue;
            if (!current.directory)
                throw std::runtime_error("Not a folder on the Kindle: " + current.name);
            auto entry = lookup(device_.children(storage_, current.id), part);
            if (!entry)
                throw std::runtime_error("No such file on the Kindle: " + path);
            current = *entry;
        }
        return current;
    }

    Objects children(uint32_t parent) { return device_.children(storage_, parent); }

    uint32_t upload(uint32_t parent, const std::string& name, const std::string& bytes)
    {
        TempFile file;
        if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file.handle) != bytes.size())
            throw std::runtime_error("Write temporary file");
        std::fflush(file.handle);
        lseek(file.fd(), 0, SEEK_SET);
        return device_.upload(storage_, parent, name, file.fd(), bytes.size(), std::time(nullptr));
    }

    uint32_t upload_blank(uint32_t parent, const std::string& name, uint64_t size)
    {
        TempFile file;
        if (ftruncate(file.fd(), static_cast<off_t>(size)) != 0)
            throw std::system_error(errno, std::generic_category(), "Size temporary file");
        return device_.upload(storage_, parent, name, file.fd(), size, std::time(nullptr));
    }

    uint32_t upload_file(uint32_t parent, const std::string& name, int fd, uint64_t size)
    {
        return device_.upload(storage_, parent, name, fd, size, std::time(nullptr));
    }

    std::string download(uint32_t id)
    {
        TempFile file;
        device_.download(id, file.fd());
        lseek(file.fd(), 0, SEEK_SET);
        std::string bytes;
        char buffer[65536];
        for (ssize_t n; (n = read(file.fd(), buffer, sizeof buffer)) != 0;) {
            if (n < 0)
                throw std::system_error(errno, std::generic_category(), "Read temporary file");
            bytes.append(buffer, static_cast<size_t>(n));
        }
        return bytes;
    }

    void download(uint32_t id, int fd) { device_.download(id, fd); }

    // Objects the Kindle deleted on its side can linger in the MTP view and refuse deletion.
    void erase_quietly(uint32_t id)
    {
        try {
            device_.erase(id);
        } catch (const std::exception&) {
        }
    }

private:
    uint32_t user_storage()
    {
        std::optional<uint32_t> found;
        for (const auto& storage : device_.storages()) {
            if (!storage.writable)
                continue;
            auto entry = lookup(device_.children(storage.id, mtp::root), "documents");
            if (!entry || !entry->directory)
                continue;
            if (found)
                throw std::runtime_error("Multiple writable Kindle user storages");
            found = storage.id;
        }
        if (!found)
            throw std::runtime_error("Could not find the Kindle's user storage");
        return *found;
    }

    mtp::Device device_;
    uint32_t storage_;
};

struct Exchange {
    uint32_t shell, in, out;
};

Exchange exchange(Kindle& kindle)
{
    auto shell = kindle.folder(mtp::root, "vector-shell");
    return {shell, kindle.folder(shell, "in"), kindle.folder(shell, "out")};
}

std::string unique_name()
{
    static int sequence = 0;
    return "c" + std::to_string(std::time(nullptr)) + "-" + std::to_string(getpid()) + "-" +
           std::to_string(sequence++);
}

struct Result {
    int status;
    std::string output;
    bool truncated;
};

Result execute(Kindle& kindle, const Exchange& folders, const std::string& script, int timeout,
               size_t capacity)
{
    const auto name = unique_name();
    const auto script_id = kindle.upload(folders.in, name + ".sh", script);
    const auto output_id = kindle.upload(folders.out, name + ".out", std::string(capacity, ' '));
    const auto status_id = kindle.upload(folders.out, name + ".rc", std::string(rc_capacity, ' '));
    // Vector Shell only runs a script after its marker arrives, so it never reads a partial upload.
    const auto marker_id = kindle.upload(folders.in, name + ".go", "go\n");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    std::string status;
    while ((status = kindle.download(status_id)).empty() || status[0] == ' ') {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("No result for " + name + " within " + std::to_string(timeout) +
                                     " s. Tap Vector Shell on the Kindle and check out/server.log.");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    int code = 0;
    unsigned long long length = 0;
    if (!(std::istringstream(status) >> code >> length))
        throw std::runtime_error("Unreadable result for " + name + ": " + status);
    auto output = kindle.download(output_id);
    bool truncated = length > output.size();
    output.resize(std::min<size_t>(output.size(), length));
    for (auto id : {marker_id, script_id, status_id, output_id})
        kindle.erase_quietly(id);
    return {code, output, truncated};
}

int run(const std::string& script, int timeout, size_t capacity)
{
    Kindle kindle;
    auto result = execute(kindle, exchange(kindle), script, timeout, capacity);
    std::cout << result.output << std::flush;
    if (result.truncated)
        std::cerr << "kshell: output truncated to " << capacity << " bytes; raise it with -o\n";
    return result.status;
}

int pull(const std::string& source, const std::string& destination, int timeout)
{
    Kindle kindle;
    auto folders = exchange(kindle);
    auto size = execute(kindle, folders, "wc -c <" + shell_quote(source) + "\n", timeout, rc_capacity);
    if (size.status != 0)
        throw std::runtime_error("Could not read " + source + ": " + size.output);
    const auto bytes = std::stoull(size.output);

    const auto name = unique_name() + ".bin";
    const auto copy_id = kindle.upload_blank(folders.out, name, bytes);
    auto copied = execute(kindle, folders,
                          "dd if=" + shell_quote(source) + " of=\"$VECTOR_SHELL/out/" + name +
                              "\" bs=65536 conv=notrunc 2>&1\n",
                          timeout, 4096);
    if (copied.status != 0) {
        kindle.erase_quietly(copy_id);
        throw std::runtime_error("Could not copy " + source + ": " + copied.output);
    }

    int fd = open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "Open " + destination);
    try {
        kindle.download(copy_id, fd);
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
    kindle.erase_quietly(copy_id);
    return 0;
}

int get(const std::string& source, const std::string& destination)
{
    Kindle kindle;
    auto object = kindle.resolve(source);
    if (object.directory)
        throw std::runtime_error("Is a folder on the Kindle: " + source);
    int fd = open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "Open " + destination);
    try {
        kindle.download(object.id, fd);
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
    return 0;
}

int put(const std::string& source, const std::string& directory)
{
    Kindle kindle;
    auto folder = kindle.resolve(directory);
    if (!folder.directory)
        throw std::runtime_error("Not a folder on the Kindle: " + directory);
    auto slash = source.find_last_of('/');
    auto name = slash == std::string::npos ? source : source.substr(slash + 1);
    int fd = open(source.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "Open " + source);
    auto size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    try {
        if (auto existing = lookup(kindle.children(folder.id), name))
            kindle.erase_quietly(existing->id);
        kindle.upload_file(folder.id, name, fd, static_cast<uint64_t>(size));
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
    return 0;
}

int list(const std::string& directory)
{
    Kindle kindle;
    auto folder = kindle.resolve(directory);
    if (!folder.directory)
        throw std::runtime_error("Not a folder on the Kindle: " + directory);
    for (const auto& entry : kindle.children(folder.id))
        std::cout << (entry.directory ? "d " : "- ") << entry.size << '\t' << entry.name << '\n';
    return 0;
}

int clean()
{
    Kindle kindle;
    auto folders = exchange(kindle);
    for (auto parent : {folders.in, folders.out})
        for (const auto& entry : kindle.children(parent))
            if (entry.name != "server.log" && entry.name != "status")
                kindle.erase_quietly(entry.id);
    return 0;
}

int stop()
{
    Kindle kindle;
    auto folders = exchange(kindle);
    if (auto existing = lookup(kindle.children(folders.shell), "stop"))
        kindle.erase_quietly(existing->id);
    kindle.upload(folders.shell, "stop", "stop\n");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        const auto command = args.empty() ? std::string() : args[0];
        if (command == "-h" || command == "--help") {
            std::cout << usage_text;
            return 0;
        }
        if (command == "pull" && args.size() == 3)
            return pull(args[1], args[2], 600);
        if (command == "get" && args.size() == 3)
            return get(args[1], args[2]);
        if (command == "put" && args.size() == 3)
            return put(args[1], args[2]);
        if (command == "ls" && args.size() <= 2)
            return list(args.size() == 2 ? args[1] : "/");
        if (command == "clean" && args.size() == 1)
            return clean();
        if (command == "stop" && args.size() == 1)
            return stop();

        int timeout = 120;
        size_t capacity = 256 * 1024;
        size_t first = 0;
        while (first + 1 < args.size() && (args[first] == "-t" || args[first] == "-o")) {
            if (args[first] == "-t")
                timeout = std::stoi(args[first + 1]);
            else
                capacity = std::stoul(args[first + 1]);
            first += 2;
        }
        std::string script;
        if (first < args.size()) {
            for (size_t i = first; i < args.size(); ++i)
                script += (i > first ? " " : "") + args[i];
            script += '\n';
        } else {
            script.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        }
        if (script.empty()) {
            std::cerr << usage_text;
            return 2;
        }
        return run(script, timeout, capacity);
    } catch (const std::exception& e) {
        std::cerr << "kshell: " << e.what() << '\n';
        return 1;
    }
}
