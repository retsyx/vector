#include "mtp_device.hpp"

#include <libmtp.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <string_view>
#include <system_error>

namespace mtp {
namespace {

constexpr uint16_t amazon_vendor = 0x1949;
constexpr uint16_t kindle_scribe_product = 0x9981;
// DEVICE_FLAG_FORCE_RESET_ON_CLOSE from libmtp's device-flags.h.
constexpr uint32_t force_reset_on_close = 0x10000000;

struct Address {
    uint32_t bus;
    uint32_t device;
};

Address parse_address(std::string_view selector)
{
    auto separator = selector.find(':');
    if (separator != std::string_view::npos) {
        Address address{};
        auto bus = std::from_chars(selector.data(), selector.data() + separator, address.bus);
        auto device = std::from_chars(selector.data() + separator + 1,
                                      selector.data() + selector.size(), address.device);
        if (bus.ec == std::errc{} && bus.ptr == selector.data() + separator &&
            device.ec == std::errc{} && device.ptr == selector.data() + selector.size() &&
            address.device <= UINT8_MAX)
            return address;
    }
    throw std::system_error(EINVAL, std::generic_category(), "Expected a USB address in bus:device form");
}

struct Free {
    void operator()(void* bytes) const { LIBMTP_FreeMemory(bytes); }
};

using RawDevices = std::unique_ptr<LIBMTP_raw_device_t[], Free>;
using File = std::unique_ptr<LIBMTP_file_t, decltype(&LIBMTP_destroy_file_t)>;
using DeviceHandle = std::unique_ptr<LIBMTP_mtpdevice_t, decltype(&LIBMTP_Release_Device)>;

int library_errno(LIBMTP_error_number_t error)
{
    switch (error) {
    case LIBMTP_ERROR_MEMORY_ALLOCATION: return ENOMEM;
    case LIBMTP_ERROR_NO_DEVICE_ATTACHED: return ENODEV;
    case LIBMTP_ERROR_STORAGE_FULL: return ENOSPC;
    case LIBMTP_ERROR_CONNECTING: return EBUSY;
    case LIBMTP_ERROR_CANCELLED: return ECANCELED;
    default: return EIO;
    }
}

int protocol_errno(const char* message)
{
    // libmtp publishes the protocol response in the error text rather than its public enum.
    constexpr std::string_view prefix = "PTP Layer error ";
    std::string_view text = message ? message : "";
    if (text.size() < prefix.size() + 4 || text.substr(0, prefix.size()) != prefix)
        return EIO;
    const char* code = text.data() + prefix.size();
    unsigned int response = 0;
    if (std::from_chars(code, code + 4, response, 16).ptr != code + 4)
        return EIO;
    switch (response) {
    case 0x02f9: // No USB device.
    case 0x2003: // Session not open.
        return ENODEV;
    case 0x02fa: return ETIMEDOUT;
    case 0x2005: return ENOTSUP; // Operation not supported.
    case 0x2008: // Invalid storage ID.
    case 0x2009: // Invalid object handle.
    case 0x201a: // Invalid parent object.
        return ESTALE;
    case 0x200c: return ENOSPC; // Store full.
    case 0x200d: return EPERM;  // Object write-protected.
    case 0x200e: return EROFS;  // Store read-only.
    case 0x200f: return EACCES; // Access denied.
    case 0x2013: return ENXIO;  // Store not available.
    case 0x2019: return EBUSY;  // Device busy.
    case 0x201d: return EINVAL; // Invalid parameter.
    case 0x201f: return ECANCELED; // Transaction canceled.
    case 0xa809: return EFBIG; // MTP object too large.
    default: return EIO;
    }
}

std::system_error device_error(LIBMTP_mtpdevice_t* device, const char* operation, int fallback = EIO)
{
    int error = fallback;
    std::string message = operation;
    const auto* first = LIBMTP_Get_Errorstack(device);
    if (first && first->error_text)
        message += std::string(" (") + first->error_text + ')';
    for (auto* entry = first; entry; entry = entry->next) {
        int mapped = entry->errornumber == LIBMTP_ERROR_PTP_LAYER
            ? protocol_errno(entry->error_text) : library_errno(entry->errornumber);
        if (mapped != EIO) {
            error = mapped;
            break;
        }
    }
    LIBMTP_Clear_Errorstack(device);
    return std::system_error(error, std::generic_category(), message);
}

RawDevices detect(int& count)
{
    static std::once_flag initialized;
    std::call_once(initialized, LIBMTP_Init);

    LIBMTP_raw_device_t* raw = nullptr;
    auto result = LIBMTP_Detect_Raw_Devices(&raw, &count);
    RawDevices devices(raw);
    if (result != LIBMTP_ERROR_NONE && result != LIBMTP_ERROR_NO_DEVICE_ATTACHED)
        throw std::system_error(library_errno(result), std::generic_category(), "Could not scan USB for MTP devices");
    return devices;
}

std::string usb_name(const LIBMTP_device_entry_t& entry)
{
    std::string name = entry.vendor ? entry.vendor : "";
    if (entry.product && *entry.product) {
        if (!name.empty())
            name += ' ';
        name += entry.product;
    }
    return name.empty() ? "MTP Device" : name;
}

void validate_name(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\0') != std::string::npos)
        throw std::system_error(EINVAL, std::generic_category(), "Invalid MTP object name");
}

uint64_t known_space(uint64_t space)
{
    return space == UINT64_MAX ? 0 : space;
}

} // namespace

struct Device::Session {
    DeviceHandle handle{nullptr, LIBMTP_Release_Device};
    std::string name;
};

void Device::list(std::ostream& output)
{
    int count = 0;
    auto devices = detect(count);
    for (int index = 0; index < count; ++index) {
        const auto& device = devices[index];
        output << device.bus_location << ':' << static_cast<unsigned int>(device.devnum)
               << '\t' << usb_name(device.device_entry) << '\n';
    }
}

Device::Device(std::optional<std::string> selector, DeviceFilter filter)
    : session_(std::make_unique<Session>())
{
    std::optional<Address> address;
    if (selector)
        address = parse_address(*selector);

    int count = 0;
    auto devices = detect(count);
    LIBMTP_raw_device_t* selected = nullptr;
    for (int index = 0; index < count; ++index) {
        auto& candidate = devices[index];
        const auto& entry = candidate.device_entry;
        if (filter && !filter(entry.vendor_id, entry.product_id, entry.product))
            continue;
        if (address && (candidate.bus_location != address->bus || candidate.devnum != address->device))
            continue;
        if (selected)
            throw std::system_error(EINVAL, std::generic_category(), filter
                ? "Connect exactly one matching MTP device"
                : "Multiple MTP devices are connected. Run --list and select one with --device");
        selected = &candidate;
    }
    if (!selected)
        throw std::system_error(ENODEV, std::generic_category(), filter
            ? "Connect and unlock a matching MTP device in USB File Transfer mode"
            : "Connect and unlock an MTP device in USB File Transfer mode");

    // Closing the Scribe session normally avoids resetting its USB connection.
    if (selected->device_entry.vendor_id == amazon_vendor &&
        selected->device_entry.product_id == kindle_scribe_product)
        selected->device_entry.device_flags &= ~force_reset_on_close;

    session_->handle.reset(LIBMTP_Open_Raw_Device_Uncached(selected));
    auto* handle = session_->handle.get();
    if (!handle)
        throw std::system_error(EBUSY, std::generic_category(),
                                "Could not open the MTP device. Quit other USB transfer apps and reconnect it");

    std::unique_ptr<char, Free> name(LIBMTP_Get_Friendlyname(handle));
    if (!name || !*name.get())
        name.reset(LIBMTP_Get_Modelname(handle));
    session_->name = name && *name.get() ? name.get() : usb_name(selected->device_entry);
    LIBMTP_Clear_Errorstack(handle);
}

Device::~Device() = default;

std::string Device::name() const
{
    return session_->name;
}

std::vector<Storage> Device::storages()
{
    auto* handle = session_->handle.get();
    LIBMTP_Clear_Errorstack(handle);
    if (LIBMTP_Get_Storage(handle, LIBMTP_STORAGE_SORTBY_NOTSORTED) < 0)
        throw device_error(handle, "Could not query MTP storage. Unlock the device and allow USB file access");

    std::vector<Storage> result;
    for (auto* item = handle->storage; item; item = item->next) {
        const char* label = item->StorageDescription;
        if (!label || !*label)
            label = item->VolumeIdentifier;
        // PTP storage IDs without a logical part and ROM storage aren't writable.
        bool writable = item->AccessCapability == 0 && (item->id & 0xffff) != 0 &&
                        item->StorageType != 1 && item->StorageType != 3;
        result.push_back({item->id, label && *label ? label : "Storage " + std::to_string(item->id),
                          known_space(item->MaxCapacity), known_space(item->FreeSpaceInBytes), writable});
    }
    return result;
}

std::vector<Object> Device::children(uint32_t storage, uint32_t parent)
{
    auto* handle = session_->handle.get();
    LIBMTP_Clear_Errorstack(handle);
    uint32_t* ids = nullptr;
    int count = LIBMTP_Get_Children(handle, storage, parent, &ids);
    std::unique_ptr<uint32_t[], Free> entries(ids);
    if (count < 0)
        throw device_error(handle, "Could not list the MTP directory");

    std::vector<Object> result;
    result.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        LIBMTP_Clear_Errorstack(handle);
        File file(LIBMTP_Get_Filemetadata(handle, entries[index]), LIBMTP_destroy_file_t);
        if (!file || !file->filename)
            throw device_error(handle, "Could not read an MTP directory entry");
        result.push_back({file->item_id, file->filename, file->filesize, file->modificationdate,
                          file->filetype == LIBMTP_FILETYPE_FOLDER});
    }
    return result;
}

void Device::download(uint32_t id, int fd)
{
    if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0)
        throw std::system_error(errno, std::generic_category(), "Could not prepare the download file");

    auto* handle = session_->handle.get();
    LIBMTP_Clear_Errorstack(handle);
    if (LIBMTP_Get_File_To_File_Descriptor(handle, id, fd, nullptr, nullptr) != 0)
        throw device_error(handle, "Could not download the MTP file");
}

uint32_t Device::upload(uint32_t storage, uint32_t parent, const std::string& name,
                        int fd, uint64_t size, std::time_t modified)
{
    validate_name(name);
    if (lseek(fd, 0, SEEK_SET) < 0)
        throw std::system_error(errno, std::generic_category(), "Could not rewind the upload file");

    File file(LIBMTP_new_file_t(), LIBMTP_destroy_file_t);
    if (!file || !(file->filename = strdup(name.c_str())))
        throw std::system_error(ENOMEM, std::generic_category(), "Could not allocate MTP file metadata");
    file->storage_id = storage;
    // SendObjectInfo uses UINT32_MAX to request the storage root explicitly.
    file->parent_id = parent;
    file->filesize = size;
    file->modificationdate = modified;
    file->filetype = LIBMTP_FILETYPE_UNKNOWN;

    auto* handle = session_->handle.get();
    LIBMTP_Clear_Errorstack(handle);
    if (LIBMTP_Send_File_From_File_Descriptor(handle, fd, file.get(), nullptr, nullptr) != 0 ||
        file->item_id == 0 || file->item_id == root) {
        auto error = device_error(handle, "Could not upload the MTP file");
        if (file->item_id != 0 && file->item_id != root && error.code().value() != ENODEV)
            LIBMTP_Delete_Object(handle, file->item_id);
        throw error;
    }

    // libmtp's generic upload sets the current time; some devices accept a correction.
    if (modified > 0) {
        std::tm utc{};
        char timestamp[32];
        if (gmtime_r(&modified, &utc) && std::strftime(timestamp, sizeof(timestamp), "%Y%m%dT%H%M%SZ", &utc))
            LIBMTP_Set_Object_String(handle, file->item_id, LIBMTP_PROPERTY_DateModified, timestamp);
    }
    return file->item_id;
}

uint32_t Device::mkdir(uint32_t storage, uint32_t parent, const std::string& name)
{
    validate_name(name);
    std::string writable_name = name;
    auto* handle = session_->handle.get();
    LIBMTP_Clear_Errorstack(handle);
    uint32_t id = LIBMTP_Create_Folder(handle, writable_name.data(), parent, storage);
    if (id == 0 || id == root)
        throw device_error(handle, "Could not create the MTP directory");
    return id;
}

void Device::erase(uint32_t id)
{
    // PTP interprets UINT32_MAX as a request to delete all matching objects.
    if (id == 0 || id == root)
        throw std::system_error(EINVAL, std::generic_category(), "Invalid MTP object ID for deletion");
    auto* handle = session_->handle.get();
    LIBMTP_Clear_Errorstack(handle);
    if (LIBMTP_Delete_Object(handle, id) != 0)
        throw device_error(handle, "Could not delete the MTP object");
}

void Device::rename(uint32_t id, const std::string& name)
{
    validate_name(name);
    auto* handle = session_->handle.get();
    LIBMTP_Clear_Errorstack(handle);
    // The file metadata API also returns folders, with the format needed for a rename.
    File file(LIBMTP_Get_Filemetadata(handle, id), LIBMTP_destroy_file_t);
    if (!file)
        throw device_error(handle, "Could not find the MTP object to rename", ESTALE);
    LIBMTP_Clear_Errorstack(handle);
    if (LIBMTP_Set_File_Name(handle, file.get(), name.c_str()) != 0)
        throw device_error(handle, "Could not rename the MTP object");
}

void Device::move(uint32_t id, uint32_t storage, uint32_t parent)
{
    auto* handle = session_->handle.get();
    LIBMTP_Clear_Errorstack(handle);
    if (!LIBMTP_Check_Capability(handle, LIBMTP_DEVICECAP_MoveObject))
        throw std::system_error(ENOTSUP, std::generic_category(), "This MTP device does not support moving objects");
    if (LIBMTP_Move_Object(handle, id, storage, parent == root ? 0 : parent) != 0)
        throw device_error(handle, "Could not move the MTP object");
}

} // namespace mtp
