#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "platform/windows/DesktopSession.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <vector>

namespace HuhuCapture::Platform::Windows {
namespace {
QString objectName(HANDLE object, DWORD& error)
{
    DWORD bytes{};
    if (!object) { error = ERROR_INVALID_HANDLE; return {}; }
    const bool queried = GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &bytes) != FALSE;
    const DWORD probeError = queried ? ERROR_INVALID_DATA : GetLastError();
    if (queried || probeError != ERROR_INSUFFICIENT_BUFFER) { error = probeError; return {}; }
    if (bytes < sizeof(wchar_t) || bytes > 64 * 1024 || bytes % sizeof(wchar_t))
        { error = ERROR_INVALID_DATA; return {}; }
    std::vector<wchar_t> name(bytes / sizeof(wchar_t));
    if (!GetUserObjectInformationW(object, UOI_NAME, name.data(), bytes, &bytes)) { error = GetLastError(); return {}; }
    if (name.back() != L'\0') { error = ERROR_INVALID_DATA; return {}; }
    const auto result = QString::fromWCharArray(name.data());
    if (result.isEmpty() || result.size() > 1024) { error = ERROR_INVALID_DATA; return {}; }
    return result;
}
}

DesktopSessionResult desktopSession()
{
    DWORD session{}, error{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
        return {{}, {Desktop::Error::Unavailable, GetLastError(), QStringLiteral("The Windows login session could not be identified")}};
    const auto station = objectName(GetProcessWindowStation(), error);
    if (station.isEmpty())
        return {{}, {Desktop::Error::Unavailable, error, QStringLiteral("The Windows window station could not be identified")}};
    const auto desktop = objectName(GetThreadDesktop(GetCurrentThreadId()), error);
    if (desktop.isEmpty())
        return {{}, {Desktop::Error::Unavailable, error, QStringLiteral("The Windows desktop could not be identified")}};
    const QJsonArray values{QStringLiteral("windows"), QString::number(session), station, desktop};
    return {QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact)), {}};
}

} // namespace HuhuCapture::Platform::Windows
