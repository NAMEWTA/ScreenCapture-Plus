#pragma once

#include "services/desktop/GlobalShortcut.h"
#include <QObject>
#include <memory>

namespace HuhuCapture::Platform {

// Internal OS seam, deliberately limited to the one screenshot action.
// Shared settings, key policy and views do not live in either implementation.
class GlobalShortcutBackend : public QObject {
public:
    explicit GlobalShortcutBackend(Desktop::GlobalShortcut& owner) : QObject(&owner), owner_(owner) {}
    virtual ~GlobalShortcutBackend() = default;
    virtual void initialize() = 0;
    virtual Desktop::ShortcutSnapshot snapshot() const = 0;
    virtual void bind(quint64 request, const QStringList& keys, QWindow* parent,
                      Desktop::GlobalShortcut::Persist persist, bool allowUnparented) = 0;
    virtual void cancel(quint64 request) = 0;
    virtual Desktop::ShortcutFailure setCaptureActive(bool active) = 0;
    // Complete only after native resources and outstanding async callbacks
    // have been released. The facade also drains its own queued admissions.
    virtual void shutdown(std::function<void()> completed = {}) = 0;
    void setActionDescription(const QString& description) { description_ = description; }
protected:
    Desktop::GlobalShortcut& owner_;
    QString description_{QStringLiteral("huhucapture")};
};

std::unique_ptr<GlobalShortcutBackend> createGlobalShortcutBackend(Desktop::GlobalShortcut& owner);

} // namespace HuhuCapture::Platform

