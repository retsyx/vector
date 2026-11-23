#include <libmtp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

constexpr uint16_t amazon_vendor_id = 0x1949;
constexpr uint16_t kindle_scribe_product_id = 0x9981;
// Defined in libmtp's device-flags.h, which the library does not install.
constexpr uint32_t mtp_force_reset_on_close = 0x10000000;

void check_mtp(LIBMTP_mtpdevice_t* device, const std::string& message)
{
    auto* error = LIBMTP_Get_Errorstack(device);
    if (error) {
        std::string details = error->error_text;
        LIBMTP_Clear_Errorstack(device);
        throw std::runtime_error(message + ": " + details);
    }
}

void destroy_list(LIBMTP_file_t* file)
{
    while (file) {
        auto* next = file->next;
        LIBMTP_destroy_file_t(file);
        file = next;
    }
}

using FileList = std::unique_ptr<LIBMTP_file_t, decltype(&destroy_list)>;

FileList list_files(LIBMTP_mtpdevice_t* device, uint32_t storage, uint32_t folder)
{
    FileList files(LIBMTP_Get_Files_And_Folders(device, storage, folder), destroy_list);
    check_mtp(device, "Could not list the Kindle folder");
    return files;
}

struct Destination {
    uint32_t storage;
    uint32_t folder;
};

Destination find_destination(LIBMTP_mtpdevice_t* device, const std::string& path)
{
    std::vector<std::string> parts;
    std::istringstream stream(path);
    for (std::string part; std::getline(stream, part, '/');)
        parts.push_back(part);

    std::vector<Destination> matches;
    for (auto* storage = device->storage; storage; storage = storage->next) {
        // MTP access capability 0 denotes read/write storage.
        if (storage->AccessCapability != 0)
            continue;

        uint32_t folder = LIBMTP_FILES_AND_FOLDERS_ROOT;
        for (const auto& part : parts) {
            auto files = list_files(device, storage->id, folder);
            auto* item = files.get();
            while (item && !(item->filetype == LIBMTP_FILETYPE_FOLDER &&
                             item->filename && strcasecmp(item->filename, part.c_str()) == 0))
                item = item->next;
            folder = item ? item->item_id : 0;
            if (!folder)
                break;
        }
        if (folder)
            matches.push_back({storage->id, folder});
    }
    if (matches.size() != 1)
        throw std::runtime_error("Expected one writable /" + path + " folder on the Kindle.");
    return matches.front();
}

struct FileDescriptor {
    int value;
    ~FileDescriptor() { if (value >= 0) close(value); }
};

void upload(LIBMTP_mtpdevice_t* device, Destination dest, const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
    auto existing = list_files(device, dest.storage, dest.folder);
    for (auto* item = existing.get(); item; item = item->next) {
        if (item->filename && strcasecmp(item->filename, name.c_str()) == 0)
            throw std::runtime_error("a file or folder with that name already exists");
    }

    FileDescriptor fd{open(path.c_str(), O_RDONLY | O_NONBLOCK)};
    if (fd.value < 0)
        throw std::runtime_error(std::strerror(errno));
    struct stat info {};
    if (fstat(fd.value, &info) != 0)
        throw std::runtime_error(std::strerror(errno));
    if (!S_ISREG(info.st_mode))
        throw std::runtime_error("select a regular file");

    std::unique_ptr<LIBMTP_file_t, decltype(&LIBMTP_destroy_file_t)>
        file(LIBMTP_new_file_t(), LIBMTP_destroy_file_t);
    if (!file)
        throw std::bad_alloc();
    file->filename = strdup(name.c_str());
    if (!file->filename)
        throw std::bad_alloc();
    file->filesize = static_cast<uint64_t>(info.st_size);
    file->storage_id = dest.storage;
    file->parent_id = dest.folder;
    file->filetype = LIBMTP_FILETYPE_UNKNOWN;

    std::cout << "Copying " << name << " …" << std::endl;
    if (LIBMTP_Send_File_From_File_Descriptor(device, fd.value, file.get(), nullptr, nullptr) != 0) {
        check_mtp(device, "USB transfer failed");
        throw std::runtime_error("USB transfer failed");
    }
    std::cout << "Copied " << name << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2)
            throw std::runtime_error("Specify a Kindle destination folder.");
        LIBMTP_Init();

        LIBMTP_raw_device_t* raw = nullptr;
        int count = 0;
        const auto result = LIBMTP_Detect_Raw_Devices(&raw, &count);
        std::unique_ptr<LIBMTP_raw_device_t, decltype(&std::free)> devices(raw, std::free);
        if (result != LIBMTP_ERROR_NONE && result != LIBMTP_ERROR_NO_DEVICE_ATTACHED)
            throw std::runtime_error("Could not scan USB for MTP devices.");

        LIBMTP_raw_device_t* kindle = nullptr;
        for (int i = 0; i < count; ++i) {
            if (raw[i].device_entry.vendor_id != amazon_vendor_id)
                continue;
            if (kindle)
                throw std::runtime_error("Connect exactly one Kindle in USB file-transfer mode.");
            kindle = &raw[i];
        }
        if (!kindle)
            throw std::runtime_error("Connect a Kindle in USB file-transfer mode.");

        // libmtp groups this Scribe with Android devices and resets its USB
        // connection on release. Keep its other quirks and close MTP normally.
        if (kindle->device_entry.product_id == kindle_scribe_product_id)
            kindle->device_entry.device_flags &= ~mtp_force_reset_on_close;

        std::unique_ptr<LIBMTP_mtpdevice_t, decltype(&LIBMTP_Release_Device)>
            device(LIBMTP_Open_Raw_Device_Uncached(kindle), LIBMTP_Release_Device);
        if (!device)
            throw std::runtime_error("Could not open the Kindle. Quit other USB transfer apps and reconnect it.");
        if (LIBMTP_Get_Storage(device.get(), LIBMTP_STORAGE_SORTBY_NOTSORTED) != 0)
            throw std::runtime_error("Could not read Kindle storage. Unlock and reconnect the Kindle.");

        const auto destination = find_destination(device.get(), argv[1]);
        if (argc == 2)
            std::cout << "Found /" << argv[1] << " on the Kindle.\n";
        bool failed = false;
        for (int i = 2; i < argc; ++i) {
            try {
                upload(device.get(), destination, argv[i]);
            } catch (const std::exception& error) {
                std::cerr << "Failed to copy " << argv[i] << ": " << error.what() << '\n';
                failed = true;
            }
        }
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
