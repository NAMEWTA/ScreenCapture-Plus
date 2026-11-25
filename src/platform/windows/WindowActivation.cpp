#include "platform/WindowActivation.h"

namespace HuhuCapture::Platform {
ActivationTokenResult applyActivationToken(QWindow*, const QString&)
{ return ActivationTokenResult::NotNeeded; }
} // namespace HuhuCapture::Platform
