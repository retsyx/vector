#include "filesystem.hpp"
#include "mtp_device.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

void usage(std::ostream& output)
{
    output << "Usage: mtp_drive --list\n"
              "       mtp_drive [--device BUS:ADDRESS] [--read-only] [--kernel] MOUNTPOINT\n";
}

int invalid_argument(const std::string& message)
{
    std::cerr << "mtp_drive: " << message << '\n';
    usage(std::cerr);
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }

    try {
        if (argc == 2 && std::string_view(argv[1]) == "--list") {
            mtp::Device::list(std::cout);
            return 0;
        }

        std::optional<std::string> selector;
        std::string mountpoint;
        bool read_only = false;
        bool kernel = false;
        bool options = true;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (options && argument == "--") {
                options = false;
            } else if (options && argument == "--device") {
                if (selector || ++i == argc || std::string_view(argv[i]).substr(0, 2) == "--")
                    return invalid_argument("--device requires one BUS:ADDRESS selector");
                selector = argv[i];
            } else if (options && argument == "--read-only") {
                read_only = true;
            } else if (options && argument == "--kernel") {
                kernel = true;
            } else if (options && argument.substr(0, 1) == "-") {
                return invalid_argument("unknown option " + std::string(argument));
            } else if (mountpoint.empty()) {
                mountpoint = argument;
            } else {
                return invalid_argument("expected one mount directory");
            }
        }

        if (mountpoint.empty())
            return invalid_argument("an empty mount directory is required");

        const auto path = std::filesystem::canonical(mountpoint);
        if (!std::filesystem::is_directory(path) || !std::filesystem::is_empty(path))
            return invalid_argument("mountpoint must be an empty directory");

        mtp::Device device(selector);
        return mtp::mount(device, path.string(), read_only, kernel);
    } catch (const std::exception& error) {
        std::cerr << "mtp_drive: " << error.what() << '\n';
        return 1;
    }
}
