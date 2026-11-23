#include "kindle_device.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <string_view>
#include <thread>

extern char** environ;

namespace manga {
namespace {

using namespace std::chrono_literals;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const { return fd_; }

private:
    int fd_;
};

struct CommandOutput {
    std::array<char, 32> bytes{};
    std::size_t size = 0;

    std::string_view text() const { return {bytes.data(), size}; }
};

std::optional<CommandOutput> read_output(int fd) noexcept {
    CommandOutput output;
    ssize_t size;
    do {
        size = read(fd, output.bytes.data(), output.bytes.size());
    } while (size < 0 && errno == EINTR);
    if (size < 0 || size == static_cast<ssize_t>(output.bytes.size())) {
        return std::nullopt;
    }
    output.size = static_cast<std::size_t>(size);
    return output;
}

bool command_succeeded(pid_t pid) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    int status;
    do {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);

    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}

bool spawn_powerd_command(pid_t& pid, char* const* arguments, int writer) noexcept {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return false;
    }
    const int stdout_error = posix_spawn_file_actions_adddup2(&actions, writer, STDOUT_FILENO);
    const int stderr_error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const bool spawned = stdout_error == 0 && stderr_error == 0 &&
                         posix_spawnp(&pid, arguments[0], &actions, nullptr, arguments, environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    return spawned;
}

std::optional<CommandOutput> powerd_command(char* const* arguments) noexcept {
    int descriptors[2];
#if defined(__linux__)
    if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) {
#else
    if (pipe(descriptors) != 0) {
#endif
        return std::nullopt;
    }
    const FileDescriptor reader(descriptors[0]);
    pid_t pid;
    {
        const FileDescriptor writer(descriptors[1]);
#if !defined(__linux__)
        if (fcntl(reader.get(), F_SETFD, FD_CLOEXEC) != 0 || fcntl(reader.get(), F_SETFL, O_NONBLOCK) != 0 ||
            fcntl(writer.get(), F_SETFD, FD_CLOEXEC) != 0) {
            return std::nullopt;
        }
#endif
        if (!spawn_powerd_command(pid, arguments, writer.get())) {
            return std::nullopt;
        }
    }
    return command_succeeded(pid) ? read_output(reader.get()) : std::nullopt;
}

std::string_view trim_space(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::optional<int> parse_level(const std::optional<CommandOutput>& output, int maximum) noexcept {
    if (!output) {
        return std::nullopt;
    }
    auto text = trim_space(output->text());
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        text = trim_space(text.substr(1, text.size() - 2));
    }
    if (text.empty()) {
        return std::nullopt;
    }
    int value;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < 0 || value > maximum) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> read_powerd_level(char* property, int maximum) noexcept {
    char program[] = "lipc-get-prop";
    char service[] = "com.lab126.powerd";
    char* arguments[] = {program, service, property, nullptr};
    return parse_level(powerd_command(arguments), maximum);
}

}  // namespace

std::optional<int> read_kindle_frontlight() noexcept {
    char property[] = "flIntensity";
    return read_powerd_level(property, kindle_frontlight_max);
}

bool set_kindle_frontlight(int level) noexcept {
    if (level < 0 || level > kindle_frontlight_max) {
        return false;
    }
    char program[] = "lipc-set-prop";
    char service[] = "com.lab126.powerd";
    char property[] = "flIntensity";
    // Reserve a null terminator after the decimal value.
    std::array<char, 4> value{};
    std::to_chars(value.data(), value.data() + value.size() - 1, level);
    char* arguments[] = {program, service, property, value.data(), nullptr};
    return powerd_command(arguments).has_value();
}

std::optional<int> read_kindle_battery_percent() noexcept {
    char property[] = "battLevel";
    if (const auto battery = read_powerd_level(property, 100)) {
        return battery;
    }

    // Kernel percentage as a fallback when the powerd property cannot be queried.
    const FileDescriptor capacity(open("/sys/class/power_supply/bd71827_bat/capacity", O_RDONLY | O_CLOEXEC));
    return capacity.get() >= 0 ? parse_level(read_output(capacity.get()), 100) : std::nullopt;
}

}  // namespace manga
