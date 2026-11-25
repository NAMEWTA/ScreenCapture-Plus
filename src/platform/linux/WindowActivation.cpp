#include "platform/linux/WindowActivation.h"
#include "WaylandActivation.h"
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

namespace HuhuCapture::Platform::Linux {
struct WindowActivation::State { WaylandActivation::Manager manager; };
WindowActivation::WindowActivation(QObject* parent) : QObject(parent), state_(std::make_unique<State>())
{
    connect(&state_->manager, &QWaylandClientExtension::activeChanged, this, &WindowActivation::availabilityChanged);
}
WindowActivation::~WindowActivation() = default;
bool WindowActivation::submit(QWindow* window, const QString& token)
{
    if (!window || token.isEmpty() || !state_->manager.isActive()) return false;
    auto* native = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!native || !native->surface()) return false;
    state_->manager.activate(token, native->surface());
    return true;
}
}
