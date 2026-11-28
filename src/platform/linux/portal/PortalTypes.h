#pragma once

#include <QMetaType>
#include <QString>

namespace HuhuCapture::Platform::Linux {

struct PortalFailure {
    enum class Code {
        None, Cancelled, ParentDestroyed, Denied, Unavailable,
        Unsupported, Busy, InvalidState, Failed
    };
    Code code{Code::None};
    QString domain;
    int nativeCode{};
    QString remoteError;
    QString message;
    explicit operator bool() const noexcept { return code != Code::None; }
};

struct PortalFeature {
    quint32 version{};
    PortalFailure failure;
    bool available() const noexcept { return version != 0 && !failure; }
};

} // namespace HuhuCapture::Platform::Linux

Q_DECLARE_METATYPE(HuhuCapture::Platform::Linux::PortalFailure)
