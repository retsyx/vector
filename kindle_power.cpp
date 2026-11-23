#include "kindle_power.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

extern char** environ;

namespace manga {
namespace {

[[noreturn]] void fail(int error, const char* operation) {
    throw std::system_error(error, std::generic_category(), operation);
}

struct PowerPipe {
    int descriptors[2] = {-1, -1};

    PowerPipe() {
#if defined(__linux__)
        if (pipe2(descriptors, O_CLOEXEC) != 0) {
            fail(errno, "Open Kindle power event pipe");
        }
#else
        if (pipe(descriptors) != 0) {
            fail(errno, "Open Kindle power event pipe");
        }
        for (int descriptor : descriptors) {
            if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
                const int error = errno;
                close(descriptors[0]);
                close(descriptors[1]);
                fail(error, "Set close-on-exec on Kindle power event pipe");
            }
        }
#endif
    }

    ~PowerPipe() {
        for (int descriptor : descriptors) {
            if (descriptor >= 0) {
                close(descriptor);
            }
        }
    }
};

bool child_finished(pid_t pid) noexcept {
    int status;
    pid_t result;
    do {
        result = waitpid(pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    return result != 0;
}

bool reap_child_briefly(pid_t pid) noexcept {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (child_finished(pid)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return child_finished(pid);
}

int spawn_listener(pid_t& pid, char* program, char** arguments, posix_spawn_file_actions_t& actions) {
    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error != 0) {
        return error;
    }

    // The viewer may block termination signals for signalfd. The child handles them directly.
    sigset_t child_mask;
    error = sigemptyset(&child_mask) == 0 ? posix_spawnattr_setsigmask(&attributes, &child_mask) : errno;
    if (error == 0) {
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
    }
    if (error == 0) {
        error = posix_spawnp(&pid, program, &actions, &attributes, arguments, environ);
    }
    posix_spawnattr_destroy(&attributes);
    return error;
}

}  // namespace

KindlePowerListener::KindlePowerListener() {
    PowerPipe pipe;
    const int reader = pipe.descriptors[0];
    const int writer = pipe.descriptors[1];
    if (fcntl(reader, F_SETFL, O_NONBLOCK) != 0) {
        fail(errno, "Set nonblocking mode on Kindle power event pipe");
    }

    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error != 0) {
        fail(error, "Initialize Kindle power event listener");
    }

    char program[] = "lipc-wait-event";
    char repeat[] = "-m";
    char option[] = "-s";
    char interval[] = "0";
    char service[] = "com.lab126.powerd";
    char events[] = "goingToScreenSaver,readyToSuspend,outOfScreenSaver";
    char* arguments[] = {program, repeat, option, interval, service, events, nullptr};

    error = posix_spawn_file_actions_addclose(&actions, reader);
    if (error == 0) {
        error = posix_spawn_file_actions_adddup2(&actions, writer, STDOUT_FILENO);
    }
    if (error == 0 && writer != STDOUT_FILENO) {
        error = posix_spawn_file_actions_addclose(&actions, writer);
    }
    if (error == 0) {
        error = spawn_listener(pid_, program, arguments, actions);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        fail(error, "Start Kindle power event listener (lipc-wait-event)");
    }

    fd_ = std::exchange(pipe.descriptors[0], -1);
}

bool KindlePowerListener::read_events() {
    char bytes[256];
    for (;;) {
        const auto size = read(fd_, bytes, sizeof(bytes));
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        if (size <= 0) {
            return false;
        }
        pending_.append(bytes, static_cast<std::size_t>(size));
        // Each event is reported on its own line, starting with the event name.
        for (auto end = pending_.find('\n'); end != std::string::npos; end = pending_.find('\n')) {
            const auto line = std::string_view(pending_).substr(0, end);
            const auto name = line.substr(0, line.find(' '));
            if (name == "goingToScreenSaver" || name == "readyToSuspend") {
                asleep_ = slept_ = true;
            } else if (name == "outOfScreenSaver") {
                asleep_ = false;
            }
            pending_.erase(0, end + 1);
        }
    }
}

KindlePowerListener::~KindlePowerListener() {
    close(fd_);
    if (child_finished(pid_)) {
        return;
    }
    // Let the stock command unsubscribe before forcing termination on an unresponsive child.
    kill(pid_, SIGINT);
    if (!reap_child_briefly(pid_)) {
        kill(pid_, SIGKILL);
        reap_child_briefly(pid_);
    }
}

}  // namespace manga
