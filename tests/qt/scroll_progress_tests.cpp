#include "app/ScrollProgress.h"

#include <QCoreApplication>
#include <QEvent>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using HuhuCapture::Application::Detail::ScrollProgress;
using HuhuCapture::Application::Detail::ScrollProgressMailbox;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
struct ImageLifetime {
    std::atomic_int alive{}, maximum{};
    struct Buffer { ImageLifetime* owner; uchar pixels[8]{}; };
    QImage make(int value)
    {
        auto* buffer = new Buffer{this, {static_cast<uchar>(value), 0, 0, 255, 0, 0, 0, 255}};
        const int count = ++alive;
        auto previous = maximum.load();
        while (count > previous && !maximum.compare_exchange_weak(previous, count)) {}
        return QImage(static_cast<const uchar*>(buffer->pixels), 2, 1, 8, QImage::Format_ARGB32,
            [](void* context) {
                auto* owned = static_cast<Buffer*>(context);
                --owned->owner->alive;
                delete owned;
            }, buffer);
    }
};
class DispatchCounter final : public QObject {
public:
    int calls{};
protected:
    bool eventFilter(QObject*, QEvent* event) override
    { if (event->type() == QEvent::MetaCall) ++calls; return false; }
};
void dispatch(QObject& receiver)
{ QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall); }
void blockedGuiRetainsLatest()
{
    ImageLifetime images;
    QObject receiver; DispatchCounter queued; receiver.installEventFilter(&queued);
    QImage displayed; int consumed{}, lastHeight{}; QString detail;
    auto mailbox = std::make_shared<ScrollProgressMailbox>(receiver, [&](ScrollProgress update) {
        ++consumed; displayed = std::move(update.preview); lastHeight = update.height; detail = std::move(update.detail);
    });
    {
        std::jthread worker([&] {
            for (int i = 1; i <= 128; ++i)
                mailbox->publish({images.make(i), 2, i, i == 1 ? QStringLiteral("capturing") : QString()});
        });
        // Deliberately do not dispatch GUI events while the real worker runs.
        worker.join();
    }
    require(images.alive == 1 && images.maximum <= 2 && consumed == 0 && queued.calls == 0,
        "A blocked GUI retains only the latest preview plus a bounded in-flight replacement");
    dispatch(receiver); dispatch(receiver);
    require(queued.calls == 1 && consumed == 1 && lastHeight == 128 &&
        displayed.constBits()[0] == 128 && detail == QStringLiteral("capturing"),
        "Exactly one real queued callback consumes the last pixels/dimensions and retained status");
    displayed = {};
    require(images.alive == 0, "The consumed preview has no hidden queued owners");
    std::jthread next([&] { mailbox->publish({images.make(201), 2, 201, {}}); }); next.join();
    dispatch(receiver);
    require(queued.calls == 2 && consumed == 2 && displayed.constBits()[0] == 201,
        "A completed dispatch reopens one notification slot for later progress");
    mailbox->close(); displayed = {};
    require(images.alive == 0, "Closing and releasing the visible preview releases every preview allocation");
}
void stopAndOwnerDestruction()
{
    ImageLifetime images;
    auto receiver = std::make_unique<QObject>(); DispatchCounter queued; receiver->installEventFilter(&queued);
    int consumed{};
    auto mailbox = std::make_shared<ScrollProgressMailbox>(*receiver, [&](ScrollProgress) { ++consumed; });
    std::jthread worker([&] {
        for (int i = 1; i <= 128; ++i) mailbox->publish({images.make(i), 2, i, {}});
    }); worker.join();
    require(images.alive == 1, "Stopped delivery starts with one real retained preview");
    QImage complete(3, 2, QImage::Format_ARGB32); complete.fill(qRgb(13, 71, 129));
    const QImage expected = complete.copy();
    mailbox->close();
    require(images.alive == 0 && complete == expected, "Stopping releases the pending slot without touching the separate complete output");
    std::jthread late([&] { mailbox->publish({images.make(222), 2, 222, {}}); }); late.join();
    dispatch(*receiver); dispatch(*receiver);
    require(queued.calls == 1 && consumed == 0 && images.alive == 0,
        "The one old queued callback is inert after stop and late publication cannot enqueue another");
    mailbox.reset();

    auto abandoned = std::make_shared<ScrollProgressMailbox>(*receiver, [&](ScrollProgress) { ++consumed; });
    std::weak_ptr<ScrollProgressMailbox> lifetime = abandoned;
    std::jthread pending([&] { abandoned->publish({images.make(77), 2, 77, {}}); }); pending.join();
    abandoned->close(); abandoned.reset();
    require(images.alive == 0 && !lifetime.expired(), "The queued envelope holds no preview after controller teardown begins");
    receiver.reset();
    require(lifetime.expired() && consumed == 0 && complete == expected,
        "Destroying the actual QObject cancels its pending delivery and releases the mailbox");
}
}
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        blockedGuiRetainsLatest(); stopAndOwnerDestruction();
        std::cout << "PASS real worker/blocked GUI latest preview, one queued callback and terminal ownership\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
