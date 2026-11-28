#pragma once

#include "services/update/UpdateService.h"
#include "ui/qt/DesktopTypes.h"

namespace HuhuCapture::UI::Qt {
inline AboutUpdateSnapshot aboutUpdateSnapshot(const Updates::Snapshot& value)
{
    AboutUpdateSnapshot result;
    switch (value.stage) {
    case Updates::Stage::Idle: result.stage = UpdateStage::Idle; break;
    case Updates::Stage::Cancelled: result.stage = UpdateStage::Cancelled; break;
    case Updates::Stage::Checking: result.stage = UpdateStage::Checking; break;
    case Updates::Stage::Available: result.stage = UpdateStage::Available; break;
    case Updates::Stage::Downloading: result.stage = UpdateStage::Downloading; break;
    case Updates::Stage::Downloaded: result.stage = UpdateStage::Downloaded; break;
    case Updates::Stage::Failed: result.stage = UpdateStage::Failed; break;
    }
    switch (value.error) {
    case Updates::Error::None: result.error = UpdateError::None; break;
    case Updates::Error::Network: result.error = UpdateError::Network; break;
    case Updates::Error::Metadata: result.error = UpdateError::Metadata; break;
    case Updates::Error::Folder: result.error = UpdateError::Folder; break;
    case Updates::Error::Integrity: result.error = UpdateError::Integrity; break;
    case Updates::Error::Save: result.error = UpdateError::Save; break;
    case Updates::Error::DurabilityUnknown: result.error = UpdateError::DurabilityUnknown; break;
    }
    result.version = value.version; result.file = value.file;
    result.received = value.received; result.total = value.total;
    switch (value.packageKind) {
    case Updates::PackageKind::WindowsInstaller: result.installationInstructionsKey = QStringLiteral("update.installWindows"); break;
    case Updates::PackageKind::WindowsPortableZip: result.installationInstructionsKey = QStringLiteral("update.extractWindows"); break;
    case Updates::PackageKind::LinuxDeb: result.installationInstructionsKey = QStringLiteral("update.installDeb"); break;
    case Updates::PackageKind::LinuxAppImage: result.installationInstructionsKey = QStringLiteral("update.replaceAppImage"); break;
    }
    result.canDownload = !value.stopped && value.stage != Updates::Stage::Checking && value.stage != Updates::Stage::Downloading;
    result.canOpenFolder = !value.file.isEmpty();
    return result;
}
} // namespace HuhuCapture::UI::Qt
