#pragma once

#include <QString>
#include <QMetaType>
#include <cstdint>

namespace HuhuCapture::Desktop {

enum class Error {
    None, InvalidArgument, WrongThread, Busy, ChangedOnDisk, Cancelled,
    RequiresInteraction, Unavailable, Allocation, Encoding, Read,
    ClipboardRejected, PartialPublication, RecycleFailed, FileOperation, Timeout
};

struct Failure {
    Error code{Error::None};
    std::int64_t nativeCode{};
    QString detail;
    explicit operator bool() const noexcept { return code != Error::None; }
};

} // namespace HuhuCapture::Desktop

Q_DECLARE_METATYPE(HuhuCapture::Desktop::Failure)
