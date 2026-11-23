#include "mtp_device.hpp"

#include <strings.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace {

bool kindle(uint16_t vendor, uint16_t product, const char* model)
{
    if (vendor != 0x1949)
        return false;
    // libmtp also lists Amazon's Fire tablets and phone under this vendor.
    if (product == 0x9981)
        return true;
    return model && !strcasestr(model, "fire") &&
           (strcasestr(model, "kindle") || strcasestr(model, "paperwhite") ||
            strcasestr(model, "scribe") || strcasestr(model, "colorsoft") || strcasestr(model, "oasis"));
}

using Objects = std::vector<mtp::Object>;

std::optional<mtp::Object> lookup(const Objects& entries, const std::string& name, bool directory)
{
    std::optional<mtp::Object> match;
    for (const auto& object : entries) {
        if (strcasecmp(object.name.c_str(), name.c_str()) != 0)
            continue;
        if (match)
            throw std::runtime_error("Ambiguous Kindle name: " + name);
        if (object.directory != directory)
            throw std::runtime_error("Unexpected object type on the Kindle: " + name);
        match = object;
    }
    return match;
}

struct Folder {
    uint32_t storage;
    uint32_t id;
    Objects entries;
};

Folder documents(mtp::Device& device)
{
    std::optional<Folder> found;
    for (const auto& storage : device.storages()) {
        if (!storage.writable)
            continue;
        auto entry = lookup(device.children(storage.id, mtp::root), "documents", true);
        // Kindle user storage exposes /documents. Android normally exposes /Documents.
        if (!entry || entry->name != "documents")
            continue;
        if (found)
            throw std::runtime_error("Multiple writable Kindle documents folders");
        found = Folder{storage.id, entry->id, device.children(storage.id, entry->id)};
    }
    if (!found)
        throw std::runtime_error("Could not find the Kindle's writable documents folder");
    return std::move(*found);
}

Folder subfolder(mtp::Device& device, const Folder& parent, const std::string& name)
{
    auto entry = lookup(parent.entries, name, true);
    if (!entry)
        return {parent.storage, device.mkdir(parent.storage, parent.id, name), {}};
    return {parent.storage, entry->id, device.children(parent.storage, entry->id)};
}

using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

bool matches(mtp::Device& device, uint32_t object, std::FILE* source)
{
    File copy(std::tmpfile(), std::fclose);
    if (!copy)
        throw std::system_error(errno, std::generic_category(), "Create verification file");
    device.download(object, fileno(copy.get()));
    if (std::fseek(copy.get(), 0, SEEK_SET) || std::fseek(source, 0, SEEK_SET))
        throw std::system_error(errno, std::generic_category(), "Rewind verification files");

    std::array<unsigned char, 65536> input, output;
    while (true) {
        auto n = std::fread(input.data(), 1, input.size(), source);
        auto m = std::fread(output.data(), 1, output.size(), copy.get());
        if (std::ferror(source) || std::ferror(copy.get()))
            throw std::runtime_error("Could not read verification files");
        if (n != m || std::memcmp(input.data(), output.data(), n) != 0)
            return false;
        if (n == 0)
            return true;
    }
}

void install_file(mtp::Device& device, const Folder& parent, const fs::path& path)
{
    const std::string name = path.filename();
    const std::string prefix = "manga-viewer-install--" + name;
    const std::string staged_name = prefix + ".part";
    const std::string backup_name = prefix + ".previous";
    auto current = lookup(parent.entries, name, false);
    auto staged = lookup(parent.entries, staged_name, false);
    auto backup = lookup(parent.entries, backup_name, false);

    if (backup && !current) {
        // An interrupted rename left the previous version under its backup name.
        device.rename(backup->id, name);
        current = backup;
        backup.reset();
    }

    File source(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!source)
        throw std::system_error(errno, std::generic_category(), "Open " + path.string());
    // libmtp reads through the descriptor; verification reads the same stream.
    std::setbuf(source.get(), nullptr);
    const auto size = fs::file_size(path);
    if (current && current->size == size && matches(device, current->id, source.get())) {
        if (backup)
            device.erase(backup->id);
        if (staged)
            device.erase(staged->id);
        std::cout << "Already installed " << name << '\n';
        return;
    }
    // A completed prior publication may have left a backup behind.
    if (backup)
        device.erase(backup->id);

    if (staged && (staged->size != size || !matches(device, staged->id, source.get()))) {
        device.erase(staged->id);
        staged.reset();
    }
    uint32_t id;
    if (staged) {
        id = staged->id;
    } else {
        id = device.upload(parent.storage, parent.id, staged_name, fileno(source.get()), size, 0);
        if (!matches(device, id, source.get()))
            throw std::runtime_error("Kindle readback differed for " + name);
    }

    if (current)
        device.rename(current->id, backup_name);
    try {
        device.rename(id, name);
        auto published = lookup(device.children(parent.storage, parent.id), name, false);
        if (!published || published->id != id)
            throw std::runtime_error("Kindle did not publish " + name);
    } catch (...) {
        if (current) {
            try {
                // The device may have renamed the object but lost its acknowledgement.
                if (!lookup(device.children(parent.storage, parent.id), name, false))
                    device.rename(current->id, name);
            } catch (const std::exception& e) {
                std::cerr << "Retry the installation to recover " << name << ": " << e.what() << '\n';
            }
        }
        throw;
    }
    if (current)
        device.erase(current->id);
    std::cout << "Installed " << name << '\n';
}

void install_libraries(mtp::Device& device, const Folder& folder, const fs::path& directory)
{
    for (const auto& item : fs::directory_iterator(directory)) {
        if (item.is_directory()) {
            auto child = subfolder(device, folder, item.path().filename());
            install_libraries(device, child, item.path());
        } else if (item.is_regular_file()) {
            install_file(device, folder, item.path());
        } else {
            throw std::runtime_error("Unexpected runtime file: " + item.path().string());
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2)
            throw std::runtime_error("Usage: install-mtp STAGING_ROOT");
        const fs::path bundle = argv[1];
        const auto viewer = bundle / "manga-viewer" / "manga_kindle";
        const auto launcher = bundle / "Manga Viewer.sh";
        const auto libs = viewer.parent_path() / "lib";
        if (!fs::is_regular_file(viewer) || !fs::is_regular_file(launcher) ||
            !fs::is_regular_file(libs / "ld-musl-armhf.so.1"))
            throw std::runtime_error("Incomplete local Manga Viewer bundle");

        mtp::Device device(std::nullopt, kindle);
        auto docs = documents(device);
        auto app = subfolder(device, docs, "manga-viewer");
        auto lib = subfolder(device, app, "lib");
        install_libraries(device, lib, libs);
        install_file(device, app, viewer);
        install_file(device, docs, launcher);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Kindle installation failed: " << e.what() << '\n';
        return 1;
    }
}
