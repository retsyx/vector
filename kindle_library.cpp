#include "kindle_library.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace manga {
namespace {

constexpr std::string_view position_header = "MANGA-VIEWER-1\n";
constexpr std::size_t position_record_limit = 16 * 1024;

class DirectoryHandle {
public:
    explicit DirectoryHandle(int fd) : fd_(fd) {}
    ~DirectoryHandle() { close(fd_); }

    DirectoryHandle(const DirectoryHandle&) = delete;
    DirectoryHandle& operator=(const DirectoryHandle&) = delete;

    int get() const { return fd_; }

    void replace(int fd) {
        close(fd_);
        fd_ = fd;
    }

private:
    int fd_;
};

char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool archive_extension(const std::filesystem::path& path) {
    auto extension = path.extension().native();
    std::transform(extension.begin(), extension.end(), extension.begin(), ascii_lower);
    return extension == ".zip" || extension == ".cbz";
}

bool visible_name(std::string_view name) {
    return !name.empty() && name.front() != '.';
}

bool library_order(const LibraryEntry& left, const LibraryEntry& right) {
    const auto& first = left.path.native();
    const auto& second = right.path.native();
    const auto differences = std::mismatch(first.begin(), first.end(), second.begin(), second.end(),
                                           [](char a, char b) { return ascii_lower(a) == ascii_lower(b); });
    if (differences.first == first.end() || differences.second == second.end()) {
        return first.size() == second.size() ? first < second : first.size() < second.size();
    }
    return static_cast<unsigned char>(ascii_lower(*differences.first)) <
           static_cast<unsigned char>(ascii_lower(*differences.second));
}

void write_atomic(const std::filesystem::path& path, std::string_view value) {
    const auto& destination = path.native();
    std::string temporary = destination + ".XXXXXX";
    const int output = mkstemp(temporary.data());
    if (output < 0) {
        throw std::system_error(errno, std::generic_category(), "Create temporary file for " + destination);
    }

    int error = 0;
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto written = write(output, value.data() + offset, value.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            error = written < 0 ? errno : EIO;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (error == 0 && fsync(output) < 0) {
        error = errno;
    }
    if (close(output) < 0 && error == 0) {
        error = errno;
    }
    if (error == 0 && rename(temporary.c_str(), destination.c_str()) < 0) {
        error = errno;
    }
    if (error != 0) {
        unlink(temporary.c_str());
        throw std::system_error(error, std::generic_category(), "Save " + destination);
    }
}

std::optional<std::size_t> consume_number(std::string_view& record) {
    const auto separator = record.find('\n');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t result;
    const auto last = record.data() + separator;
    const auto parsed = std::from_chars(record.data(), last, result);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    record.remove_prefix(separator + 1);
    return result;
}

}  // namespace

std::vector<LibraryEntry> list_library(const std::filesystem::path& directory) {
    using namespace std::filesystem;
    std::vector<LibraryEntry> entries;
    auto entry = recursive_directory_iterator(directory, directory_options::skip_permission_denied);
    for (const auto end = recursive_directory_iterator(); entry != end; ++entry) {
        std::error_code error;
        const auto status = entry->symlink_status(error);
        if (error || !visible_name(entry->path().filename().native()) || is_symlink(status)) {
            entry.disable_recursion_pending();
        } else if (is_regular_file(status) && archive_extension(entry->path())) {
            entries.push_back({entry->path()});
        }
    }
    std::sort(entries.begin(), entries.end(), library_order);
    return entries;
}

void remove_library_archive(const std::filesystem::path& root, const std::filesystem::path& archive) {
    const auto relative = archive.lexically_relative(root);
    if (!root.is_absolute() || !archive.is_absolute() || relative.empty() || !archive_extension(relative) ||
        archive.native().find('\0') != std::string::npos ||
        std::any_of(relative.begin(), relative.end(), [](const std::filesystem::path& part) {
            return !visible_name(part.native());
        })) {
        throw std::invalid_argument("Expected a ZIP or CBZ file inside the library");
    }

    // Walk from the filesystem root with pinned directory handles so a replaced symlink
    // cannot redirect the deletion between the picker and confirmation.
    constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    const auto filesystem_root = open("/", flags);
    if (filesystem_root < 0) {
        throw std::system_error(errno, std::generic_category(), "Open filesystem root");
    }
    DirectoryHandle directory(filesystem_root);
    const auto parent = archive.parent_path().relative_path();
    for (const auto& part : parent) {
        const auto next = openat(directory.get(), part.c_str(), flags);
        if (next < 0) {
            throw std::system_error(errno, std::generic_category(), "Open archive directory " + parent.string());
        }
        directory.replace(next);
    }

    const auto name = archive.filename();
    struct stat status {};
    if (fstatat(directory.get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) < 0) {
        throw std::system_error(errno, std::generic_category(), "Inspect archive " + archive.string());
    }
    if (!S_ISREG(status.st_mode)) {
        throw std::invalid_argument("Expected a regular ZIP or CBZ file");
    }
    if (unlinkat(directory.get(), name.c_str(), 0) < 0) {
        throw std::system_error(errno, std::generic_category(), "Delete archive " + archive.string());
    }
}

TapDirection read_tap_direction(const std::filesystem::path& settings) noexcept {
    try {
        std::error_code error;
        if (std::filesystem::is_regular_file(std::filesystem::symlink_status(settings, error))) {
            std::ifstream input(settings);
            std::array<char, 16> bytes{};
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            const std::string_view value(bytes.data(), static_cast<std::size_t>(input.gcount()));
            if (input.eof() && (value == "right\n" || value == "right" || value == "right\r\n")) {
                return TapDirection::right_advances;
            }
        }
    } catch (...) {
        // An unavailable setting should not prevent the reader from starting.
    }
    return TapDirection::left_advances;
}

void write_tap_direction(const std::filesystem::path& settings, TapDirection direction) {
    write_atomic(settings, direction == TapDirection::right_advances ? "right\n" : "left\n");
}

std::optional<ReadingPosition> read_reading_position(const std::filesystem::path& state) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(state, error))) {
            return std::nullopt;
        }

        std::ifstream input(state, std::ios::binary);
        std::array<char, position_record_limit> bytes{};
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::string_view record(bytes.data(), static_cast<std::size_t>(input.gcount()));
        if (!input.eof() || input.bad() || record.substr(0, position_header.size()) != position_header) {
            return std::nullopt;
        }
        record.remove_prefix(position_header.size());

        const auto page = consume_number(record);
        const auto path_size = consume_number(record);
        if (!page || !path_size || record.empty() || record.size() != *path_size ||
            record.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        return ReadingPosition{std::filesystem::path(record), *page};
    } catch (...) {
        return std::nullopt;
    }
}

void write_reading_position(const std::filesystem::path& state, const ReadingPosition& position) {
    const auto& archive = position.archive.native();
    // Keeping the path as a length-prefixed byte sequence preserves spaces and line breaks.
    auto record = std::string(position_header) + std::to_string(position.page_index) + '\n' +
                  std::to_string(archive.size()) + '\n';
    if (archive.empty() || archive.find('\0') != std::string::npos ||
        archive.size() >= position_record_limit - record.size()) {
        throw std::invalid_argument("Invalid archive path for the saved position");
    }
    record += archive;
    write_atomic(state, record);
}

}  // namespace manga
