#include "heif_loader.hpp"

#include <libheif/heif.h>
#include <png.h>
#include <turbojpeg.h>
#include <zip.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace manga {
namespace {

constexpr std::size_t max_input_bytes = 64 * 1024 * 1024;
constexpr int heif_size_limit = 8192;
constexpr std::uint64_t max_pixels = std::uint64_t{heif_size_limit} * heif_size_limit;
constexpr std::size_t channels = 4;
constexpr std::string_view digits = "0123456789";

void check_dimensions(std::int64_t width, std::int64_t height) {
    if (width <= 0 || height <= 0 ||
        static_cast<std::uint64_t>(width) > max_pixels / static_cast<std::uint64_t>(height)) {
        throw std::runtime_error("Invalid or oversized image dimensions");
    }
}

Image allocate_image(std::int64_t width, std::int64_t height) {
    check_dimensions(width, height);
    const auto byte_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * channels;
    return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
            std::vector<std::uint8_t>(byte_count)};
}

void check_heif(heif_error error, const char* operation) {
    if (error.code != heif_error_Ok) {
        throw std::runtime_error(std::string(operation) + ": " + error.message);
    }
}

struct HeifSession {
    HeifSession() { check_heif(heif_init(nullptr), "Initialize libheif"); }
    ~HeifSession() { heif_deinit(); }
};

void unpremultiply_alpha(Image& image) {
    for (std::size_t offset = 0; offset < image.pixels.size(); offset += channels) {
        auto* pixel = image.pixels.data() + offset;
        const unsigned alpha = pixel[3];
        if (alpha == 255) {
            continue;
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto straight = alpha == 0 ? 0u : (pixel[channel] * 255u + alpha / 2) / alpha;
            pixel[channel] = static_cast<std::uint8_t>(std::min(255u, straight));
        }
    }
}

Image decode_png(const std::vector<std::uint8_t>& bytes) {
    png_image decoder{};
    decoder.version = PNG_IMAGE_VERSION;
    const std::unique_ptr<png_image, decltype(&png_image_free)> cleanup(&decoder, png_image_free);
    if (!png_image_begin_read_from_memory(&decoder, bytes.data(), bytes.size())) {
        throw std::runtime_error(std::string("Read PNG header: ") + decoder.message);
    }

    auto result = allocate_image(decoder.width, decoder.height);
    decoder.format = PNG_FORMAT_RGBA;
    if (!png_image_finish_read(&decoder, nullptr, result.pixels.data(), 0, nullptr)) {
        throw std::runtime_error(std::string("Decode PNG: ") + decoder.message);
    }
    return result;
}

Image decode_jpeg(const std::vector<std::uint8_t>& bytes) {
    const std::unique_ptr<void, decltype(&tj3Destroy)> decoder(tj3Init(TJINIT_DECOMPRESS), tj3Destroy);
    if (!decoder) {
        throw std::runtime_error("Initialize JPEG decoder");
    }
    if (tj3Set(decoder.get(), TJPARAM_STOPONWARNING, 1) != 0 ||
        tj3DecompressHeader(decoder.get(), bytes.data(), bytes.size()) != 0) {
        throw std::runtime_error(std::string("Read JPEG header: ") + tj3GetErrorStr(decoder.get()));
    }

    auto result = allocate_image(tj3Get(decoder.get(), TJPARAM_JPEGWIDTH),
                                 tj3Get(decoder.get(), TJPARAM_JPEGHEIGHT));
    if (tj3Decompress8(decoder.get(), bytes.data(), bytes.size(), result.pixels.data(), 0, TJPF_RGBA) != 0) {
        throw std::runtime_error(std::string("Decode JPEG: ") + tj3GetErrorStr(decoder.get()));
    }
    return result;
}

enum class PageFormat { other, heif, png, jpeg };

PageFormat page_format(std::string_view name) {
    if (name.empty() || name.back() == '/') {
        return PageFormat::other;
    }
    const auto extension_start = name.find_last_of('.');
    if (extension_start == std::string_view::npos) {
        return PageFormat::other;
    }
    std::string extension(name.substr(extension_start));
    for (char& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    if (extension == ".heif" || extension == ".heic" || extension == ".avif") {
        return PageFormat::heif;
    }
    if (extension == ".png") {
        return PageFormat::png;
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return PageFormat::jpeg;
    }
    return PageFormat::other;
}

bool is_macos_metadata(std::string_view path) {
    while (!path.empty()) {
        const auto separator = path.find('/');
        const auto component = path.substr(0, separator);
        if (component == "__MACOSX" || component.substr(0, 2) == "._") {
            return true;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        path.remove_prefix(separator + 1);
    }
    return false;
}

std::string_view next_number(std::string_view& name) {
    const auto length = std::min(name.find_first_not_of(digits), name.size());
    auto number = name.substr(0, length);
    name.remove_prefix(length);
    number.remove_prefix(std::min(number.find_first_not_of('0'), number.size()));
    return number;
}

bool natural_less(std::string_view left, std::string_view right) {
    const auto original_left = left;
    const auto original_right = right;
    while (!left.empty() && !right.empty()) {
        if (digits.find(left.front()) != std::string_view::npos &&
            digits.find(right.front()) != std::string_view::npos) {
            const auto left_number = next_number(left);
            const auto right_number = next_number(right);
            if (left_number.size() != right_number.size()) {
                return left_number.size() < right_number.size();
            }
            if (left_number != right_number) {
                return left_number < right_number;
            }
        } else {
            if (left.front() != right.front()) {
                return left.substr(0, 1) < right.substr(0, 1);
            }
            left.remove_prefix(1);
            right.remove_prefix(1);
        }
    }
    if (left.size() != right.size()) {
        return left.empty();
    }
    return original_left < original_right;
}

std::vector<std::uint8_t> read_entry(zip_t* archive, zip_uint64_t index) {
    zip_stat_t status;
    if (zip_stat_index(archive, index, 0, &status) != 0) {
        throw std::runtime_error(zip_strerror(archive));
    }
    if (!(status.valid & ZIP_STAT_SIZE) || status.size == 0 || status.size > max_input_bytes) {
        throw std::runtime_error("ZIP entry is empty, oversized or missing its size");
    }

    const std::unique_ptr<zip_file_t, decltype(&zip_fclose)> entry(zip_fopen_index(archive, index, 0), zip_fclose);
    if (!entry) {
        throw std::runtime_error(zip_strerror(archive));
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.size));
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = zip_fread(entry.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            throw std::runtime_error(zip_file_strerror(entry.get()));
        }
        if (count == 0) {
            throw std::runtime_error("Truncated ZIP entry");
        }
        offset += static_cast<std::size_t>(count);
    }

    // Read through EOF so libzip checks the entry's CRC.
    std::uint8_t extra;
    const auto count = zip_fread(entry.get(), &extra, 1);
    if (count < 0) {
        throw std::runtime_error(zip_file_strerror(entry.get()));
    }
    if (count != 0) {
        throw std::runtime_error("ZIP entry exceeds its declared size");
    }
    return bytes;
}

}  // namespace

Image decode_heif(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0 || size > max_input_bytes) {
        throw std::runtime_error("HEIF input is empty or oversized");
    }

    const HeifSession session;
    const std::unique_ptr<heif_context, decltype(&heif_context_free)> context(heif_context_alloc(), heif_context_free);
    if (!context) {
        throw std::runtime_error("Allocate HEIF decoder context");
    }
    heif_context_set_maximum_image_size_limit(context.get(), heif_size_limit);
    check_heif(heif_context_read_from_memory_without_copy(context.get(), data, size, nullptr), "Read HEIF");

    heif_image_handle* primary = nullptr;
    check_heif(heif_context_get_primary_image_handle(context.get(), &primary), "Find primary HEIF image");
    const std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle(primary, heif_image_handle_release);
    check_dimensions(heif_image_handle_get_width(handle.get()), heif_image_handle_get_height(handle.get()));

    heif_image* decoded = nullptr;
    check_heif(heif_decode_image(handle.get(), &decoded, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr),
               "Decode HEIF");
    const std::unique_ptr<heif_image, decltype(&heif_image_release)> image(decoded, heif_image_release);

    const auto width = heif_image_get_width(image.get(), heif_channel_interleaved);
    const auto height = heif_image_get_height(image.get(), heif_channel_interleaved);
    check_dimensions(width, height);
    const auto row_bytes = static_cast<std::size_t>(width) * channels;
    int plane_stride = 0;
    const auto* plane = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &plane_stride);
    const auto stride = static_cast<std::size_t>(plane_stride);
    if (!plane || plane_stride <= 0 || stride < row_bytes ||
        static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / stride) {
        throw std::runtime_error("Invalid HEIF pixel plane");
    }

    auto result = allocate_image(width, height);
    for (std::size_t row = 0; row < result.height; ++row) {
        std::memcpy(result.pixels.data() + row * row_bytes, plane + row * stride, row_bytes);
    }
    if (heif_image_is_premultiplied_alpha(image.get())) {
        unpremultiply_alpha(result);
    }
    return result;
}

void ZipBook::ArchiveCloser::operator()(zip* archive) const noexcept {
    zip_discard(archive);
}

ZipBook::ZipBook(const std::filesystem::path& path) {
    const auto filename = path.string();
    int error_code = 0;
    archive_.reset(zip_open(filename.c_str(), ZIP_RDONLY, &error_code));
    if (!archive_) {
        zip_error_t error;
        zip_error_init_with_code(&error, error_code);
        const std::unique_ptr<zip_error_t, decltype(&zip_error_fini)> cleanup(&error, zip_error_fini);
        throw std::runtime_error(filename + ": " + zip_error_strerror(&error));
    }

    const auto count = zip_get_num_entries(archive_.get(), 0);
    if (count < 0) {
        throw std::runtime_error(filename + ": " + zip_strerror(archive_.get()));
    }
    for (zip_int64_t index = 0; index < count; ++index) {
        const auto entry_index = static_cast<zip_uint64_t>(index);
        const char* name = zip_get_name(archive_.get(), entry_index, 0);
        if (!name) {
            throw std::runtime_error(filename + ": " + zip_strerror(archive_.get()));
        }
        if (page_format(name) != PageFormat::other && !is_macos_metadata(name)) {
            pages_.push_back({name, entry_index});
        }
    }
    std::stable_sort(pages_.begin(), pages_.end(), [](const Page& left, const Page& right) {
        return natural_less(left.name, right.name);
    });
}

std::size_t ZipBook::page_count() const noexcept {
    return pages_.size();
}

const std::string& ZipBook::page_name(std::size_t page_index) const {
    return pages_.at(page_index).name;
}

Image ZipBook::load_page(std::size_t page_index) const {
    const auto& page = pages_.at(page_index);
    try {
        std::vector<std::uint8_t> bytes;
        {
            const std::lock_guard<std::mutex> lock(archive_mutex_);
            bytes = read_entry(archive_.get(), page.entry_index);
        }
        const auto format = page_format(page.name);
        if (format == PageFormat::png) {
            return decode_png(bytes);
        }
        if (format == PageFormat::jpeg) {
            return decode_jpeg(bytes);
        }
        return decode_heif(bytes.data(), bytes.size());
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(page.name + ": " + error.what());
    }
}

}  // namespace manga
