#pragma once

#include <sys/types.h>

namespace manga {

class KindlePowerListener {
public:
    KindlePowerListener();
    ~KindlePowerListener();

    KindlePowerListener(const KindlePowerListener&) = delete;
    KindlePowerListener& operator=(const KindlePowerListener&) = delete;

    int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
    pid_t pid_ = -1;
};

}  // namespace manga
