#include "ui/qt/AboutPanel.h"

#include "app/Brand.h"
#include "ui/qt/DesktopTheme.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QVBoxLayout>
#include <algorithm>

namespace HuhuCapture::UI::Qt {
namespace {
namespace Theme = DesktopTheme;
QString brand(std::wstring_view value) { return QString::fromWCharArray(value.data(), static_cast<qsizetype>(value.size())); }
}

struct AboutPanel::State {
    DesktopTranslator translator;
    AboutUpdateSnapshot update;
    bool linksEnabled{};
    QLabel *heading{}, *description{}, *versionKey{}, *version{}, *projectKey{}, *authorKey{}, *status{};
    QWidget* versionRow{};
    Theme::ElidingLabel* path{};
    QPushButton *project{}, *author{}, *download{}, *folder{}, *releases{};
    QProgressBar* progress{};
};

AboutPanel::AboutPanel(QWidget* parent) : QWidget(parent), state_(std::make_unique<State>())
{
    setObjectName(QStringLiteral("settingsAboutPage"));
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(0);
    state_->heading = Theme::label(this, 22); state_->heading->setFont(Theme::font(22, QFont::DemiBold));
    state_->description = Theme::label(this, 12, true); state_->description->setWordWrap(true);
    outer->addWidget(state_->heading); outer->addSpacing(2); outer->addWidget(state_->description); outer->addSpacing(16);
    auto* information = Theme::card(this); auto* details = new QVBoxLayout(information);
    details->setContentsMargins(18, 12, 18, 4); details->setSpacing(0);
    auto* name = Theme::label(information, 20); name->setFont(Theme::font(20, QFont::DemiBold)); name->setText(brand(Brand::name));
    details->addWidget(name); details->addSpacing(4);
    const auto row = [&](QLabel*& label) {
        details->addWidget(Theme::divider(information));
        auto* result = new QWidget(information); result->setMinimumHeight(44);
        auto* line = new QHBoxLayout(result); line->setContentsMargins(0, 0, 0, 0); line->setSpacing(12);
        label = Theme::label(result); line->addWidget(label, 1); details->addWidget(result); return result;
    };
    state_->versionRow = row(state_->versionKey);
    state_->version = Theme::label(state_->versionRow); state_->version->setObjectName(QStringLiteral("settingsVersion"));
    state_->version->setTextInteractionFlags(::Qt::TextSelectableByMouse);
    state_->version->setAlignment(::Qt::AlignRight | ::Qt::AlignVCenter);
    static_cast<QHBoxLayout*>(state_->versionRow->layout())->addWidget(state_->version);
    auto* project = row(state_->projectKey); state_->project = Theme::button(project, "settingsProject");
    state_->project->setText(brand(Brand::repositoryLabel)); state_->project->setProperty("link", true);
    static_cast<QHBoxLayout*>(project->layout())->addWidget(state_->project);
    auto* author = row(state_->authorKey); state_->author = Theme::button(author, "settingsAuthor");
    state_->author->setText(brand(Brand::author)); state_->author->setProperty("link", true);
    static_cast<QHBoxLayout*>(author->layout())->addWidget(state_->author);
    outer->addWidget(information); outer->addSpacing(16);
    auto* actions = new QFormLayout; actions->setContentsMargins(0, 0, 0, 0); actions->setSpacing(8);
    actions->setRowWrapPolicy(QFormLayout::WrapLongRows); actions->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    state_->download = Theme::button(this, "settingsDownloadUpdate", true);
    auto* secondary = new QWidget(this); auto* secondaryLayout = new QFormLayout(secondary);
    secondaryLayout->setContentsMargins(0, 0, 0, 0); secondaryLayout->setSpacing(8);
    secondaryLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    secondaryLayout->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    state_->folder = Theme::button(secondary, "settingsUpdateFolder");
    state_->releases = Theme::button(secondary, "settingsReleases"); state_->releases->setProperty("link", true);
    secondaryLayout->addRow(state_->folder, state_->releases); actions->addRow(state_->download, secondary);
    outer->addLayout(actions); outer->addSpacing(8);
    state_->progress = new QProgressBar(this); state_->progress->setObjectName(QStringLiteral("settingsUpdateProgress"));
    state_->progress->setFixedHeight(6); state_->progress->setTextVisible(false);
    outer->addWidget(state_->progress); outer->addSpacing(4);
    state_->status = Theme::label(this, 12, true); state_->status->setObjectName(QStringLiteral("settingsUpdateStatus"));
    state_->status->setWordWrap(true); outer->addWidget(state_->status);
    state_->path = new Theme::ElidingLabel(this); state_->path->setFont(Theme::font(12)); state_->path->setProperty("secondary", true);
    state_->path->setObjectName(QStringLiteral("settingsUpdatePath")); outer->addSpacing(4); outer->addWidget(state_->path); outer->addStretch();
    connect(state_->project, &QPushButton::clicked, this, [this] { if (state_->linksEnabled) Q_EMIT openLinkRequested(QUrl(brand(Brand::repositoryUrl))); });
    connect(state_->author, &QPushButton::clicked, this, [this] { if (state_->linksEnabled) Q_EMIT openLinkRequested(QUrl(brand(Brand::authorUrl))); });
    connect(state_->releases, &QPushButton::clicked, this, [this] { if (state_->linksEnabled) Q_EMIT openLinkRequested(QUrl(brand(Brand::releasesUrl))); });
    connect(state_->download, &QPushButton::clicked, this, [this] {
        if (state_->download->isEnabled()) Q_EMIT downloadRequested();
    });
    connect(state_->folder, &QPushButton::clicked, this, [this] {
        if (state_->folder->isEnabled()) Q_EMIT openUpdateFolderRequested(state_->update.file);
    });
    setApplicationVersion({}); setLinksEnabled(false); refreshLanguage();
}

AboutPanel::~AboutPanel() = default;
void AboutPanel::setTranslator(DesktopTranslator translator)
{ state_->translator = std::move(translator); refreshLanguage(); }
QString AboutPanel::translated(QStringView key) const
{ return state_->translator ? state_->translator(key) : key.toString(); }
void AboutPanel::setApplicationVersion(const QString& version)
{ state_->version->setText(version); state_->versionRow->setVisible(!version.isEmpty()); }
void AboutPanel::setUpdateSnapshot(const AboutUpdateSnapshot& snapshot)
{ state_->update = snapshot; refreshUpdate(); }
void AboutPanel::setLinksEnabled(bool enabled)
{
    state_->linksEnabled = enabled;
    state_->project->setEnabled(enabled); state_->author->setEnabled(enabled); state_->releases->setEnabled(enabled);
}

void AboutPanel::refreshLanguage()
{
    state_->heading->setText(translated(u"setting.about")); state_->description->setText(translated(u"about.description"));
    state_->versionKey->setText(translated(u"about.version")); state_->projectKey->setText(translated(u"about.project"));
    state_->authorKey->setText(translated(u"about.author")); state_->releases->setText(translated(u"about.releases"));
    state_->folder->setText(translated(u"update.openFolder")); refreshUpdate();
}

void AboutPanel::refreshUpdate()
{
    const auto& update = state_->update;
    const bool busy = update.stage == UpdateStage::Checking || update.stage == UpdateStage::Downloading;
    const bool downloaded = update.stage == UpdateStage::Downloaded && !update.file.isEmpty();
    const bool failed = update.stage == UpdateStage::Failed || (update.stage == UpdateStage::Downloaded && !downloaded);
    state_->download->setEnabled(update.canDownload && !busy);
    state_->download->setText(translated(failed ? u"update.retry" : u"update.download"));
    state_->folder->setVisible(downloaded); state_->folder->setEnabled(downloaded && update.canOpenFolder);
    state_->folder->setToolTip(update.file); state_->path->setFullText(update.file); state_->path->setVisible(downloaded);
    state_->progress->setVisible(update.stage == UpdateStage::Downloading);
    state_->status->setProperty("error", failed); Theme::repolish(state_->status);
    switch (update.stage) {
    case UpdateStage::Idle: state_->status->setText(translated(u"update.idle")); break;
    case UpdateStage::Checking:
        state_->download->setText(translated(u"update.checking")); state_->status->setText(translated(u"update.idle")); break;
    case UpdateStage::Available: state_->status->setText(translated(u"update.found") + QStringLiteral(" v") + update.version); break;
    case UpdateStage::Downloading: {
        const auto bounded = (std::min)(update.received, update.total);
        const int percent = update.total ? static_cast<int>(static_cast<long double>(bounded) * 100 / update.total) : 0;
        state_->progress->setRange(0, update.total ? 100 : 0); state_->progress->setValue(percent);
        state_->download->setText(translated(u"update.downloading") + (update.total ? QStringLiteral(" %1%").arg(percent) : QString()));
        state_->status->setText(QStringLiteral("v") + update.version + QStringLiteral(" · ") + translated(u"update.idle")); break;
    }
    case UpdateStage::Downloaded:
        state_->status->setText(downloaded ? QStringLiteral("v") + update.version + QStringLiteral(" · ") +
            translated(u"update.downloaded") : translated(u"update.saveError")); break;
    case UpdateStage::Failed: {
        const auto* key = update.error == UpdateError::Metadata ? u"update.metadataError" :
            update.error == UpdateError::Folder ? u"update.folderError" : update.error == UpdateError::Integrity ? u"update.integrityError" :
            update.error == UpdateError::Save ? u"update.saveError" : u"update.networkError";
        state_->status->setText(translated(key)); break;
    }
    }
}

} // namespace HuhuCapture::UI::Qt
