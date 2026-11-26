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
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace manga {
namespace {

constexpr std::string_view positions_header = "MANGA-VIEWER-3\n";
// Version 2 records hold the position of a single archive and the saved view.
constexpr std::string_view position_header = "MANGA-VIEWER-2\n";
// Version 1 records predate the saved view and always reopened the archive.
constexpr std::string_view reading_position_header = "MANGA-VIEWER-1\n";
constexpr std::string_view reading_view = "reading\n";
constexpr std::string_view library_view = "library\n";
// Only archives in the library keep a saved page, so this leaves room for thousands of them.
constexpr std::size_t position_record_limit = 1024 * 1024;
constexpr std::string_view cover_header = "MANGA-VIEWER-COVER-1\n";
constexpr std::string_view cover_extension = ".cover";
constexpr std::string_view hex_digits = "0123456789abcdef";
// Cover file names start with a 64-bit hash of the archive's path in hexadecimal.
constexpr std::size_t cover_name_digits = 16;
// Room for the archive's path and the numbers that precede the pixels of a cover.
constexpr std::size_t cover_header_limit = 16 * 1024;

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

bool consume_prefix(std::string_view& record, std::string_view prefix) {
    if (record.substr(0, prefix.size()) != prefix) {
        return false;
    }
    record.remove_prefix(prefix.size());
    return true;
}

template <typename Number = std::size_t>
std::optional<Number> consume_number(std::string_view& record) {
    const auto separator = record.find('\n');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    Number result;
    const auto last = record.data() + separator;
    const auto parsed = std::from_chars(record.data(), last, result);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    record.remove_prefix(separator + 1);
    return result;
}

// Keeping a path as a length-prefixed byte sequence preserves spaces and line breaks. A line break follows it.
std::optional<std::filesystem::path> consume_path(std::string_view& record) {
    const auto size = consume_number(record);
    if (!size || *size >= record.size() || record[*size] != '\n') {
        return std::nullopt;
    }
    const auto path = record.substr(0, *size);
    if (path.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    record.remove_prefix(*size + 1);
    return std::filesystem::path(path);
}

void append_path(std::string& record, const std::filesystem::path& path) {
    const auto& bytes = path.native();
    if (bytes.find('\0') != std::string::npos) {
        throw std::invalid_argument("Invalid archive path for the saved positions");
    }
    record += std::to_string(bytes.size()) + '\n';
    record += bytes;
    record += '\n';
}

std::optional<ReadingPositions> parse_positions(std::string_view record) {
    auto reading = consume_path(record);
    const auto count = consume_number(record);
    if (!reading || !count) {
        return std::nullopt;
    }
    ReadingPositions positions;
    positions.reading = std::move(*reading);
    for (std::size_t index = 0; index < *count; ++index) {
        const auto page = consume_number(record);
        auto archive = consume_path(record);
        if (!page || !archive || archive->empty()) {
            return std::nullopt;
        }
        positions.pages.emplace(std::move(*archive), *page);
    }
    if (!record.empty()) {
        return std::nullopt;
    }
    return positions;
}

// Versions 1 and 2 end with the page and the unterminated path of a single archive.
std::optional<ReadingPositions> parse_single_position(std::string_view record, bool reading) {
    const auto page = consume_number(record);
    const auto path_size = consume_number(record);
    if (!page || !path_size || record.empty() || record.size() != *path_size ||
        record.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    ReadingPositions positions;
    const std::filesystem::path archive(record);
    if (*page != 0) {
        positions.pages.emplace(archive, *page);
    }
    if (reading) {
        positions.reading = archive;
    }
    return positions;
}

// 64-bit FNV-1a.
std::uint64_t hash_bytes(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037u;
    for (const char byte : bytes) {
        hash = (hash ^ static_cast<unsigned char>(byte)) * 1099511628211u;
    }
    return hash;
}

struct ArchiveStamp {
    std::uint64_t size;
    // Nanoseconds since the epoch.
    std::uint64_t modified;
};

ArchiveStamp archive_stamp(const std::filesystem::path& archive) {
    struct stat status {};
    if (stat(archive.c_str(), &status) < 0) {
        throw std::system_error(errno, std::generic_category(), "Inspect archive " + archive.native());
    }
    return {static_cast<std::uint64_t>(status.st_size),
            static_cast<std::uint64_t>(status.st_mtim.tv_sec) * 1000000000u +
                static_cast<std::uint64_t>(status.st_mtim.tv_nsec)};
}

// Matches cover files and the temporary files of interrupted saves.
bool cover_file_name(std::string_view name) {
    return name.size() >= cover_name_digits + cover_extension.size() &&
           name.find_first_not_of(hex_digits) >= cover_name_digits &&
           name.substr(cover_name_digits, cover_extension.size()) == cover_extension;
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

ReadingPositions read_reading_positions(const std::filesystem::path& state) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(state, error))) {
            return {};
        }
        const auto size = std::filesystem::file_size(state, error);
        if (error || size > position_record_limit) {
            return {};
        }

        std::ifstream input(state, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(input.gcount()) != bytes.size() ||
            input.peek() != std::ifstream::traits_type::eof()) {
            return {};
        }

        std::string_view record(bytes);
        std::optional<ReadingPositions> positions;
        if (consume_prefix(record, positions_header)) {
            positions = parse_positions(record);
        } else if (consume_prefix(record, position_header)) {
            const bool reading = consume_prefix(record, reading_view);
            if (reading || consume_prefix(record, library_view)) {
                positions = parse_single_position(record, reading);
            }
        } else if (consume_prefix(record, reading_position_header)) {
            positions = parse_single_position(record, true);
        }
        return std::move(positions).value_or(ReadingPositions{});
    } catch (...) {
        return {};
    }
}

void write_reading_positions(const std::filesystem::path& state, const ReadingPositions& positions) {
    auto record = std::string(positions_header);
    append_path(record, positions.reading);
    record += std::to_string(positions.pages.size()) + '\n';
    for (const auto& [archive, page] : positions.pages) {
        if (archive.empty()) {
            throw std::invalid_argument("Invalid archive path for the saved positions");
        }
        record += std::to_string(page) + '\n';
        append_path(record, archive);
    }
    if (record.size() > position_record_limit) {
        throw std::length_error("Too many saved reading positions");
    }
    write_atomic(state, record);
}

void forget_removed_archives(ReadingPositions& positions, const std::filesystem::path& root,
                             const std::vector<LibraryEntry>& entries) {
    decltype(positions.pages) kept;
    for (const auto& entry : entries) {
        const auto saved = positions.pages.find(entry.path.lexically_relative(root));
        if (saved != positions.pages.end()) {
            kept.insert(positions.pages.extract(saved));
        }
    }
    positions.pages = std::move(kept);
}

CoverCache::CoverCache(std::filesystem::path directory, std::filesystem::path root)
    : directory_(std::move(directory)), root_(std::move(root)) {}

std::filesystem::path CoverCache::file(const std::filesystem::path& archive) const {
    const auto hash = hash_bytes(archive.lexically_relative(root_).native());
    std::string name(cover_name_digits, '0');
    for (std::size_t index = 0; index < cover_name_digits; ++index) {
        name[index] = hex_digits[(hash >> (4 * (cover_name_digits - 1 - index))) & 0xF];
    }
    name += cover_extension;
    return directory_ / name;
}

std::vector<std::uint8_t> CoverCache::read(const std::filesystem::path& archive, int width,
                                           int height) const noexcept {
    try {
        if (width <= 0 || height <= 0) {
            return {};
        }
        const auto stamp = archive_stamp(archive);
        const auto pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const auto path = file(archive);
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > pixel_count + cover_header_limit) {
            return {};
        }
        std::ifstream input(path, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
            return {};
        }

        std::string_view record(bytes);
        if (!consume_prefix(record, cover_header)) {
            return {};
        }
        const auto relative = consume_path(record);
        const auto archive_size = consume_number<std::uint64_t>(record);
        const auto modified = consume_number<std::uint64_t>(record);
        const auto cover_width = consume_number(record);
        const auto cover_height = consume_number(record);
        const auto checksum = consume_number<std::uint64_t>(record);
        if (relative != archive.lexically_relative(root_) || archive_size != stamp.size ||
            modified != stamp.modified || cover_width != static_cast<std::size_t>(width) ||
            cover_height != static_cast<std::size_t>(height) || !checksum || record.size() != pixel_count ||
            hash_bytes(record) != *checksum) {
            return {};
        }
        const auto* pixels = reinterpret_cast<const std::uint8_t*>(record.data());
        return {pixels, pixels + record.size()};
    } catch (...) {
        return {};
    }
}

void CoverCache::write(const std::filesystem::path& archive, int width, int height,
                       const std::vector<std::uint8_t>& pixels) const {
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        throw std::invalid_argument("The cover does not match its size");
    }
    const auto stamp = archive_stamp(archive);
    const std::string_view bytes(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    auto record = std::string(cover_header);
    append_path(record, archive.lexically_relative(root_));
    for (const std::uint64_t number : {stamp.size, stamp.modified, static_cast<std::uint64_t>(width),
                                       static_cast<std::uint64_t>(height), hash_bytes(bytes)}) {
        record += std::to_string(number) + '\n';
    }
    record += bytes;
    if (mkdir(directory_.c_str(), 0755) < 0 && errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), "Create cover directory " + directory_.native());
    }
    write_atomic(file(archive), record);
}

void CoverCache::remove(const std::filesystem::path& archive) const noexcept {
    try {
        std::error_code error;
        std::filesystem::remove(file(archive), error);
    } catch (...) {
        // A cover left behind goes when the library next opens without its archive.
    }
}

void CoverCache::forget_removed_archives(const std::vector<LibraryEntry>& entries) const noexcept {
    try {
        std::set<std::string> current;
        for (const auto& entry : entries) {
            current.insert(file(entry.path).filename().native());
        }
        std::vector<std::filesystem::path> removed;
        std::error_code error;
        for (std::filesystem::directory_iterator cover(directory_, error), end; !error && cover != end;
             cover.increment(error)) {
            const auto name = cover->path().filename().native();
            if (cover_file_name(name) && current.count(name) == 0) {
                removed.push_back(cover->path());
            }
        }
        for (const auto& path : removed) {
            std::filesystem::remove(path, error);
        }
    } catch (...) {
        // Covers that could not be deleted are tried again when the library next opens.
    }
}

}  // namespace manga
