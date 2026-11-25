#pragma once

#include "services/localization/LanguageCatalog.h"
#include "services/media/MediaArchiveService.h"

#include <QStringList>
#include <QUrl>
#include <functional>

namespace HuhuCapture::UI::Qt {

using DesktopTranslator = std::function<QString(QStringView)>;
enum class DesktopResult { Succeeded, Cancelled, Failed };

// Capabilities describe a real connected service. Windows and Linux use the
// same widgets; the host reports OS availability and any localized explanation.
enum class ShortcutEditing { Unavailable, LocalCapture, SystemDialog };
struct SettingsSnapshot {
    bool autoStart{};
    bool canChangeAutoStart{};
    bool canChangeLanguage{};
    bool canOpenArchive{};
    bool canOpenLinks{};
    bool canOpenLanSettings{};
    QString autoStartUnavailableReason;
    QString languageCode;
    QString effectiveLanguageCode;
    QVector<Localization::LanguageEntry> languages;
    QString archiveDirectory;
    QString captureShortcut;
    ShortcutEditing shortcutEditing{ShortcutEditing::Unavailable};
    QString shortcutUnavailableReason;
    QString shortcutCommand;
};

enum class SettingField { AutoStart, Language, CaptureShortcut };
struct SettingChange {
    SettingField field{SettingField::AutoStart};
    bool autoStart{};
    QString languageCode;
    // Canonical Ctrl/Alt/Shift/Win/key tokens, preserving legacy storage names.
    // An empty list requests the system's shortcut chooser, never an unbind.
    QStringList shortcutKeys;
};

enum class UpdateStage { Idle, Checking, Available, Downloading, Downloaded, Failed };
enum class UpdateError { None, Network, Metadata, Folder, Integrity, Save };
struct AboutUpdateSnapshot {
    UpdateStage stage{UpdateStage::Idle};
    UpdateError error{UpdateError::None};
    QString version;
    QString file;
    QString installationInstructionsKey{QStringLiteral("update.downloaded")};
    quint64 received{};
    quint64 total{};
    bool canDownload{};
    bool canOpenFolder{};
};

enum class HistoryCommand { OpenFolder, Preview, Copy, Reveal, Send };
struct HistoryCapabilities {
    bool reload{};
    bool openFolder{};
    bool preview{};
    bool copy{};
    bool reveal{};
    bool send{};
    bool clear{};
};

} // namespace HuhuCapture::UI::Qt

Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::DesktopResult)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::SettingsSnapshot)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::SettingChange)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::AboutUpdateSnapshot)
Q_DECLARE_METATYPE(HuhuCapture::UI::Qt::HistoryCommand)
Q_DECLARE_METATYPE(HuhuCapture::Media::Entry)
Q_DECLARE_METATYPE(HuhuCapture::Media::ListResult)
