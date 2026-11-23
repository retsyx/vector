#pragma once

#include <sys/types.h>

#include <string>

namespace manga {

// Follows the Kindle screensaver through powerd events.
class KindlePowerListener {
public:
    KindlePowerListener();
    ~KindlePowerListener();

    KindlePowerListener(const KindlePowerListener&) = delete;
    KindlePowerListener& operator=(const KindlePowerListener&) = delete;

    int fd() const noexcept { return fd_; }

    // Reads the events reported so far. Returns false once the listener has stopped.
    bool read_events();
    bool asleep() const noexcept { return asleep_; }
    // Whether the screensaver started since the last reset_sleep() call.
    bool slept() const noexcept { return slept_; }
    void reset_sleep() noexcept { slept_ = asleep_; }

private:
    int fd_ = -1;
    pid_t pid_ = -1;
    std::string pending_;
    bool asleep_ = false;
    bool slept_ = false;
};

}  // namespace manga
