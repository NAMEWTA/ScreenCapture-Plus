#pragma once

#include <QString>
#include <functional>
#include <memory>

namespace HuhuCapture::Desktop {

enum class AutoStartError {
    None, InvalidExecutable, Unavailable, ForeignEntry, Read, Write,
    ChangedOnDisk, SaveFailed, RollbackFailed,
};
struct AutoStartSnapshot {
    bool enabled{};
    bool canChange{};
    AutoStartError error{AutoStartError::None};
};
struct AutoStartResult {
    AutoStartError error{AutoStartError::None};
    AutoStartSnapshot effective;
    bool ok() const noexcept { return error == AutoStartError::None; }
};

// One application's own user-level entry only. Construction/query never write
// OS state. A user change first succeeds in the OS, then calls persist; failed
// persistence restores the exact previous entry unless an outside writer has
// already changed it. The actual post-operation state is always returned.
class AutoStart final {
public:
    explicit AutoStart(QString persistentExecutable);
    ~AutoStart();
    AutoStart(const AutoStart&) = delete;
    AutoStart& operator=(const AutoStart&) = delete;
    AutoStartSnapshot snapshot() const;
    AutoStartResult setEnabled(bool enabled, const std::function<bool()>& persist);
private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace HuhuCapture::Desktop
