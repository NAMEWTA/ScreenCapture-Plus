#pragma once

#include <QImage>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace HuhuCapture::Application::Detail {

struct ScrollProgress {
    QImage preview;
    int width{}, height{};
    QString detail;
};

// The scroll worker owns complete pixels elsewhere. This mailbox retains only
// its latest bounded preview and posts at most one pending GUI notification.
// The controller closes it before stopping/joining its worker or destroying the
// receiver. A queued callback then retains no preview or controller callback.
class ScrollProgressMailbox final : public std::enable_shared_from_this<ScrollProgressMailbox> {
public:
    using Consumer = std::function<void(ScrollProgress)>;
    ScrollProgressMailbox(QObject& receiver, Consumer consumer)
        : receiver_(&receiver), consumer_(std::make_shared<Consumer>(std::move(consumer))) {}

    void publish(ScrollProgress update)
    {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return;
            // An empty status means retain the prior status. Coalescing must
            // not drop the latest still-pending nonempty status either.
            if (latest_ && update.detail.isEmpty()) update.detail = std::move(latest_->detail);
            latest_ = std::move(update);
            if (posted_) return;
            posted_ = true;
        }
        try {
            const auto self = shared_from_this();
            if (!QMetaObject::invokeMethod(receiver_, [self] { self->dispatch(); }, Qt::QueuedConnection))
                close();
        } catch (...) {
            close();
            throw;
        }
    }

    void close() noexcept
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        latest_.reset();
        consumer_.reset();
    }

private:
    void dispatch()
    {
        std::optional<ScrollProgress> update;
        std::shared_ptr<Consumer> consumer;
        {
            std::lock_guard lock(mutex_);
            posted_ = false;
            if (closed_) return;
            update = std::move(latest_);
            latest_.reset();
            consumer = consumer_;
        }
        // Releasing the notification slot before consumption permits another
        // latest value while the GUI paints, still with at most one queued call.
        if (update && consumer) (*consumer)(std::move(*update));
    }

    QObject* const receiver_;
    std::mutex mutex_;
    std::optional<ScrollProgress> latest_;
    std::shared_ptr<Consumer> consumer_;
    bool posted_{}, closed_{};
};

} // namespace HuhuCapture::Application::Detail
