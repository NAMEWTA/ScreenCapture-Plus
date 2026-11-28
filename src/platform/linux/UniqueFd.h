#pragma once

#include <unistd.h>
#include <utility>

namespace HuhuCapture::Platform::Linux {

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int replacement = -1) noexcept
    {
        if (fd_ >= 0) ::close(fd_);
        fd_ = replacement;
    }
private:
    int fd_;
};

} // namespace HuhuCapture::Platform::Linux
