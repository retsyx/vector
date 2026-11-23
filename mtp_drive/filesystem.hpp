#pragma once

#include <string>

namespace mtp {

class Device;

int mount(Device& device, const std::string& mountpoint, bool read_only, bool kernel);

} // namespace mtp
