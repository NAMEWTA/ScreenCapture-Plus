#include "ui/qt/EditorWindow.h"

#include "ui/AnnotationPanelLayout.h"
#include "ui/toolbars/ToolbarMetrics.h"
#include <QAbstractListModel>
#include <QApplication>
#include <QCloseEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QInputMethod>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QResource>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QTextCursor>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

// Q_INIT_RESOURCE must be outside the application namespace. Referencing the
// generated symbol also retains the qrc object when this UI is a static library.
static void initializeHuhuEditorResources() { Q_INIT_RESOURCE(huhucapture_ui); }

namespace HuhuCapture::UI::Qt {
namespace {
constexpr int buttonSize = static_cast<int>(UI::toolbarButtonSize);
constexpr int iconSize = static_cast<int>(UI::toolbarIconSize);
constexpr auto index(OutputIntent intent) { return static_cast<std::size_t>(intent); }

QString iconFontFamily()
{
    static const QString family = [] {
        initializeHuhuEditorResources();
        const auto id = QFontDatabase::addApplicationFont(QStringLiteral(":/huhucapture/editor/iconfont.ttf"));
        const auto families = id < 0 ? QStringList() : QFontDatabase::applicationFontFamilies(id);
        return families.isEmpty() ? QString() : families.front();
    }();
    return family;
}

// Existing iconfont glyphs and the existing annotation path, rendered at the
// requested Qt icon/device scale. No per-platform asset or hard-coded DPR.
class EditorIcon final : public QIconEngine {
public:
    EditorIcon(QString family, char16_t glyph, bool annotation = false)
        : family_(std::move(family)), glyph_(glyph), annotation_(annotation) {}
    explicit EditorIcon(QColor color) : color_(color) {}
    QIconEngine* clone() const override { return new EditorIcon(*this); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        painter->save();
        const auto side = (std::min)(rect.width(), rect.height());
        painter->translate(rect.x() + (rect.width() - side) / 2.0, rect.y() + (rect.height() - side) / 2.0);
        painter->scale(side / 24.0, side / 24.0);
        painter->setRenderHint(QPainter::Antialiasing);
        const QColor ink = mode == QIcon::Disabled ? QColor(150, 150, 150) : QColor(51, 51, 51);
        if (color_.isValid()) {
            painter->setPen(color_ == QColor(::Qt::white) ? QPen(QColor(168, 168, 168), 1) : QPen(::Qt::NoPen));
            painter->setBrush(color_);
            painter->drawRoundedRect(QRectF(.5, .5, 23, 23), 2, 2);
            if (state == QIcon::On) {
                painter->setPen(QPen(color_ == QColor(::Qt::white) ? ::Qt::black : ::Qt::white, 1.8, ::Qt::SolidLine, ::Qt::RoundCap, ::Qt::RoundJoin));
                QPainterPath check; check.moveTo(6, 12); check.lineTo(10, 16); check.lineTo(18, 8);
                painter->drawPath(check);
            }
        } else if (annotation_) {
            painter->setBrush(::Qt::NoBrush);
            painter->setPen(QPen(ink, 1.6, ::Qt::SolidLine, ::Qt::SquareCap, ::Qt::RoundJoin));
            QPainterPath p;
            p.moveTo(5, 3); p.lineTo(19, 3); p.cubicTo(22, 3, 22, 5, 22, 6);
            p.lineTo(22, 14); p.cubicTo(22, 17, 20, 17, 19, 17);
            p.lineTo(11, 17); p.lineTo(7, 21); p.lineTo(7, 17); p.lineTo(5, 17);
            p.cubicTo(2, 17, 2, 15, 2, 14); p.lineTo(2, 6); p.cubicTo(2, 3, 4, 3, 5, 3); p.closeSubpath();
            painter->drawPath(p);
            painter->drawLine(QPointF(7, 8), QPointF(9, 6)); painter->drawLine(QPointF(9, 6), QPointF(9, 13));
            painter->drawLine(QPointF(7, 13), QPointF(11, 13));
            painter->drawLine(QPointF(14, 7), QPointF(18, 7)); painter->drawLine(QPointF(14, 11), QPointF(18, 11));
        } else {
            QFont font(family_); font.setPixelSize(24);
            painter->setFont(font); painter->setPen(ink);
            painter->drawText(QRectF(0, 0, 24, 24), ::Qt::AlignCenter, QString(QChar(glyph_)));
        }
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size); result.fill(::Qt::transparent);
        QPainter painter(&result); paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
private:
    QString family_;
    char16_t glyph_{};
    bool annotation_{};
    QColor color_;
};

class ToolbarStrip final : public QWidget {
public:
    explicit ToolbarStrip(QWidget* parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setStyleSheet(QStringLiteral("ToolbarStrip { background: white; } QToolButton { border: 0; border-radius: 0; padding: 0; background: transparent; color: #333333; } QToolButton:hover { background: #f2f2f2; } QToolButton:checked { background: #e6f4ff; } QToolButton:disabled { color: #969696; }"));
    }
    void add(QWidget* widget, int width = buttonSize)
    {
        widget->setParent(this);
        controls_.push_back(widget);
        widths_.push_back(static_cast<float>(width));
        widget->show();
        arrange();
    }
    void clear()
    {
        for (auto* control : controls_) delete control;
        controls_.clear(); widths_.clear(); arrange();
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    { return qCeil(UI::measureToolbar(widths_, static_cast<float>(width)).height); }
    QSize sizeHint() const override
    {
        const auto extent = UI::measureToolbar(widths_);
        return {qCeil(extent.width), qCeil(extent.height)};
    }
    QSize minimumSizeHint() const override { return {buttonSize + 2, heightForWidth(width())}; }
    void refreshTextWidths()
    {
        for (std::size_t i = 0; i < controls_.size(); ++i) {
            if (auto* button = qobject_cast<QToolButton*>(controls_[i]); button && button->icon().isNull())
                widths_[i] = static_cast<float>((std::max)(buttonSize, button->sizeHint().width() + 16));
        }
        arrange();
    }
protected:
    void resizeEvent(QResizeEvent*) override { arrange(); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(rect(), ::Qt::white);
        painter.setPen(QColor(168, 168, 168)); painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
private:
    void arrange()
    {
        if (controls_.empty()) { setFixedHeight(0); return; }
        const int limit = (std::max)(buttonSize, width() - 2);
        int x = 1, y = 1;
        for (std::size_t i = 0; i < controls_.size(); ++i) {
            const int w = qCeil(widths_[i]);
            if (x > 1 && x + w > limit + 1) { x = 1; y += buttonSize; }
            controls_[i]->setGeometry(x, y, w, buttonSize);
            x += w;
        }
        const int nextHeight = heightForWidth(width());
        if (height() != nextHeight) setFixedHeight(nextHeight);
        updateGeometry(); update();
    }
    std::vector<QWidget*> controls_;
    std::vector<float> widths_;
};

class ToolbarDivider final : public QWidget {
public:
    explicit ToolbarDivider(QWidget* parent) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.fillRect(QRectF(0, (height() - UI::toolbarDividerHeight) / 2,
            UI::toolbarDivider, UI::toolbarDividerHeight), QColor(220, 220, 220));
    }
};

QToolButton* makeButton(QWidget* parent, const QString& key, char16_t glyph,
    const QString& family, bool annotation = false)
{
    auto* button = new QToolButton(parent);
    button->setProperty("languageKey", key);
    button->setObjectName(key);
    button->setFocusPolicy(::Qt::NoFocus);
    button->setIconSize({iconSize, iconSize});
    if (annotation || !family.isEmpty()) button->setIcon(QIcon(new EditorIcon(family, glyph, annotation)));
    else button->setToolButtonStyle(::Qt::ToolButtonTextOnly); // Readable localized fallback, not empty boxes.
    return button;
}

class NotesModel final : public QAbstractListModel {
public:
    struct Row {
        quint64 number{};
        QString preview;
        bool empty{};
        mutable int layoutWidth{-1};
        mutable QFont font;
        mutable std::unique_ptr<QTextLayout> layout;
    };
    explicit NotesModel(QObject* parent) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        if (role == ::Qt::DisplayRole) return row.empty ? emptyText_ : row.preview;
        if (role == ::Qt::UserRole) return QVariant::fromValue(row.number);
        return {};
    }
    const Row& row(int index) const { return rows_[static_cast<std::size_t>(index)]; }
    QModelIndex find(quint64 number) const
    {
        for (int i = 0; i < rowCount(); ++i) if (row(i).number == number) return index(i);
        return {};
    }
    void setNotes(const std::vector<Core::AnnotationNote>& notes)
    {
        std::vector<Row> next;
        next.reserve(notes.size());
        for (const auto& note : notes) next.push_back(makeRow(note.number, QString::fromStdWString(note.note)));
        beginResetModel(); rows_ = std::move(next); endResetModel();
    }
    void updateNote(quint64 number, const QString& text)
    {
        const auto found = find(number);
        if (!found.isValid()) return;
        rows_[static_cast<std::size_t>(found.row())] = makeRow(number, text);
        emit dataChanged(found, found);
    }
    void setEmptyText(QString text)
    {
        emptyText_ = std::move(text);
        for (auto& row : rows_) row.layout.reset();
        if (rowCount()) emit dataChanged(index(0), index(rowCount() - 1));
    }
private:
    static Row makeRow(quint64 id, const QString& text)
    {
        // A display excerpt only; the complete string always stays in the shape.
        auto preview = text.simplified();
        const bool empty = preview.isEmpty();
        if (preview.size() > 160) preview = preview.left(159) + QChar(0x2026);
        return {id, std::move(preview), empty, -1, {}, {}};
    }
    QString emptyText_;
    std::vector<Row> rows_;
};

class NotesDelegate final : public QStyledItemDelegate {
public:
    explicit NotesDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return {200, 64}; }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const auto* model = static_cast<const NotesModel*>(index.model());
        const auto& row = model->row(index.row());
        painter->save();
        painter->setClipRect(option.rect);
        painter->setRenderHint(QPainter::Antialiasing);
        const auto bounds = option.rect.adjusted(4, 3, -4, -3);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->setPen(selected ? QPen(QColor(37, 99, 235), 1) : QPen(::Qt::NoPen));
        painter->setBrush(selected ? QColor(239, 246, 255) : QColor(::Qt::white));
        painter->drawRoundedRect(bounds, 6, 6);
        auto font = option.font; font.setPixelSize(13);
        auto badgeFont = font; badgeFont.setWeight(QFont::DemiBold);
        const QString number = QString::number(row.number);
        const int badgeWidth = qCeil((std::max)(28.0, QFontMetricsF(badgeFont).horizontalAdvance(number) + 12));
        const QRect badge(bounds.left() + 8, bounds.top() + 10, badgeWidth, 28);
        const auto color = Core::annotationColor(row.number);
        painter->setPen(::Qt::NoPen); painter->setBrush(QColor::fromRgb(color.backgroundRgb));
        painter->drawRoundedRect(badge, 14, 14);
        painter->setFont(badgeFont); painter->setPen(QColor::fromRgb(color.textRgb));
        painter->drawText(badge, ::Qt::AlignCenter, number);
        const QRect textRect(badge.right() + 10, bounds.top() + 9,
            (std::max)(1, bounds.right() - badge.right() - 18), bounds.height() - 18);
        if (!row.layout || row.layoutWidth != textRect.width() || row.font != font) {
            const auto full = index.data(::Qt::DisplayRole).toString();
            QTextLayout first(full, font);
            first.beginLayout(); auto firstLine = first.createLine();
            if (firstLine.isValid()) firstLine.setLineWidth(textRect.width());
            const int count = firstLine.isValid() ? firstLine.textLength() : 0;
            first.endLayout();
            const auto rest = full.mid(count);
            const auto second = QFontMetricsF(font).elidedText(rest, ::Qt::ElideRight, textRect.width());
            const auto visible = full.left(count) + (rest.isEmpty() ? QString() : QString(QChar(QChar::LineSeparator)) + second);
            row.layout = std::make_unique<QTextLayout>(visible, font);
            QTextOption options; options.setWrapMode(QTextOption::NoWrap);
            row.layout->setTextOption(options); row.layout->setCacheEnabled(true);
            row.layout->beginLayout(); qreal y = 0;
            for (;;) {
                auto line = row.layout->createLine(); if (!line.isValid()) break;
                line.setLineWidth(textRect.width()); line.setPosition({0, y}); y += line.height();
            }
            row.layout->endLayout(); row.layoutWidth = textRect.width(); row.font = font;
        }
        painter->setPen(row.empty ? QColor(100, 116, 139) : QColor(23, 32, 51));
        painter->setClipRect(textRect, ::Qt::IntersectClip);
        row.layout->draw(painter, textRect.topLeft());
        painter->restore();
    }
};

class NotePanel final : public QWidget {
public:
    explicit NotePanel(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("annotationPanel"));
        setMinimumHeight(240); setFixedWidth(340);
        setStyleSheet(QStringLiteral("QWidget#annotationPanel { background: #f8fafc; } QPlainTextEdit { background: white; color: #172033; border: 1px solid #cbd5e1; border-radius: 7px; padding: 8px; selection-background-color: #bfdbfe; } QPlainTextEdit:focus { border-color: #2563eb; } QToolButton { border: 0; color: #475569; padding: 4px; } QToolButton:hover { background: #e6f4ff; }"));
        title = new QLabel(this); heading = new QLabel(this); footer = new QLabel(this);
        collapse = new QToolButton(this); remove = new QToolButton(this);
        collapse->setFocusPolicy(::Qt::NoFocus); remove->setFocusPolicy(::Qt::NoFocus);
        list = new QListView(this); list->setObjectName(QStringLiteral("annotationList"));
        list->setFrameShape(QFrame::NoFrame); list->setHorizontalScrollBarPolicy(::Qt::ScrollBarAlwaysOff);
        list->setSelectionMode(QAbstractItemView::SingleSelection); list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        list->setUniformItemSizes(true);
        list->setStyleSheet(QStringLiteral("QListView { background: #f8fafc; border: none; }"));
        editor = new QPlainTextEdit(this); editor->setObjectName(QStringLiteral("annotationNoteEditor"));
        editor->setTabChangesFocus(false);
        auto textFont = font(); textFont.setPixelSize(15); editor->setFont(textFont);
        auto titleFont = font(); titleFont.setPixelSize(16); titleFont.setWeight(QFont::DemiBold); title->setFont(titleFont);
        auto small = font(); small.setPixelSize(12); footer->setFont(small);
        footer->setStyleSheet(QStringLiteral("color: #64748b;"));
    }
    QLabel *title{}, *heading{}, *footer{};
    QToolButton *collapse{}, *remove{};
    QListView* list{};
    QPlainTextEdit* editor{};
    void relayout()
    {
        arrange(); updateGeometry(); update();
    }
protected:
    void resizeEvent(QResizeEvent*) override { arrange(); }
private:
    void arrange()
    {
        const auto g = UI::annotationPanelGeometry(static_cast<float>(height()), 1.f);
        const int actionWidth = (std::max)({54, collapse->sizeHint().width(), remove->sizeHint().width()});
        title->setGeometry(14, 6, (std::max)(1, width() - actionWidth - 28), 42);
        collapse->setGeometry(width() - actionWidth - 8, 6, actionWidth, 42);
        list->setGeometry(10, qRound(g.listTop), width() - 20, qRound(g.listBottom - g.listTop));
        heading->setGeometry(14, qRound(g.editorHeadingTop), (std::max)(1, width() - actionWidth - 30), 34);
        remove->setGeometry(width() - actionWidth - 10, qRound(g.editorHeadingTop), actionWidth, 34);
        editor->setGeometry(14, qRound(g.editorTop), width() - 28, qRound(g.editorHeight));
        footer->setGeometry(14, qRound(g.footerTop), width() - 28, 22);
    }
};
}

struct EditorWindow::State {
    EditorCanvas* canvas{};
    QScrollArea* scroll{};
    NotePanel* notes{};
    NotesModel* notesModel{};
    ToolbarStrip *main{}, *style{};
    QToolButton *undo{}, *redo{}, *close{}, *save{}, *clipboard{};
    QSlider* slider{};
    std::vector<QToolButton*> mainButtons, toggles, colors;
    QLabel *status{}, *zoomLabel{};
    QTimer* zoomHide{};
    QSize editExtent;
    EditorTranslator translator;
    QString iconFamily;
    std::array<bool, 4> outputs{};
    quint64 nextToken{1}, pendingToken{}, selectedNote{};
    OutputIntent pendingIntent{OutputIntent::Clipboard};
    QPointer<QWidget> previousFocus;
    bool closeAfterOutput{true}, changingNote{}, deferredNotes{}, notesCollapsed{};
};

EditorWindow::EditorWindow(QWidget* parent)
    : QWidget(parent, ::Qt::Tool | ::Qt::FramelessWindowHint | ::Qt::WindowStaysOnTopHint), state_(std::make_unique<State>())
{
    setObjectName(QStringLiteral("editorWindow"));
    state_->iconFamily = iconFontFamily();
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(2);
    auto* content = new QWidget(this);
    auto* contentLayout = new QHBoxLayout(content); contentLayout->setContentsMargins(0, 0, 0, 0); contentLayout->setSpacing(10);
    state_->canvas = new EditorCanvas;
    state_->scroll = new QScrollArea(content); state_->scroll->setObjectName(QStringLiteral("editorScrollArea"));
    state_->scroll->setFrameShape(QFrame::NoFrame); state_->scroll->setWidgetResizable(false);
    state_->scroll->setWidget(state_->canvas); state_->scroll->setAlignment(::Qt::AlignLeft | ::Qt::AlignTop);
    state_->scroll->setFocusProxy(state_->canvas);
    contentLayout->addWidget(state_->scroll, 1);
    state_->notes = new NotePanel(content); contentLayout->addWidget(state_->notes); state_->notes->hide();
    state_->notesModel = new NotesModel(this);
    state_->notes->list->setModel(state_->notesModel);
    state_->notes->list->setItemDelegate(new NotesDelegate(this));
    state_->notes->editor->installEventFilter(this);
    state_->notes->list->installEventFilter(this);
    outer->addWidget(content, 1);
    state_->main = new ToolbarStrip(this); state_->main->setObjectName(QStringLiteral("editorMainToolbar")); outer->addWidget(state_->main);
    state_->style = new ToolbarStrip(this); state_->style->setObjectName(QStringLiteral("editorStyleToolbar")); outer->addWidget(state_->style);
    state_->status = new QLabel(this); state_->status->setObjectName(QStringLiteral("editorStatus"));
    state_->status->setWordWrap(true); state_->status->setMargin(6); state_->status->setStyleSheet(QStringLiteral("color: #a61d24; background: #fff1f0;"));
    outer->addWidget(state_->status); state_->status->hide();
    state_->zoomLabel = new QLabel(state_->scroll->viewport()); state_->zoomLabel->hide();
    state_->zoomLabel->setStyleSheet(QStringLiteral("QLabel { color: white; background: #333333; border-radius: 4px; padding: 4px 8px; }"));
    state_->zoomHide = new QTimer(this); state_->zoomHide->setSingleShot(true); state_->zoomHide->setInterval(800);
    connect(state_->zoomHide, &QTimer::timeout, state_->zoomLabel, &QWidget::hide);
    makeMainToolbar();
    setFocusProxy(state_->canvas);
    connect(state_->canvas, &EditorCanvas::toolChanged, this, [this](EditorTool tool) {
        for (auto* button : state_->mainButtons) {
            if (!button->isCheckable()) continue;
            button->setChecked(button->property("tool").toInt() == static_cast<int>(tool));
        }
        rebuildStyleToolbar();
        if (tool == EditorTool::Annotation) { state_->notesCollapsed = false; setNotesVisible(true); }
    });
    connect(state_->canvas, &EditorCanvas::toolStyleChanged, this, [this](EditorTool, EditorToolStyle) { updateStyleToolbar(); });
    connect(state_->canvas, &EditorCanvas::historyChanged, this, &EditorWindow::updateActions);
    connect(state_->canvas, &EditorCanvas::annotationsChanged, this, &EditorWindow::syncAnnotations);
    connect(state_->canvas, &EditorCanvas::annotationSelected, this, &EditorWindow::selectNote);
    connect(state_->canvas, &EditorCanvas::annotationNoteChanged, this, [this](quint64 number) {
        if (const auto* shape = state_->canvas->document().findAnnotation(number))
            state_->notesModel->updateNote(number, QString::fromStdWString(shape->annotationModel()->note()));
    });
    connect(state_->canvas, &EditorCanvas::pointerGestureChanged, this, [this](bool active) {
        if (!active && state_->deferredNotes) { state_->deferredNotes = false; syncAnnotations(); }
    });
    connect(state_->canvas, &EditorCanvas::pureImageModeChanged, this, [this](bool pure) {
        if (pure) state_->editExtent = size();
        state_->main->setVisible(!pure); state_->style->setVisible(!pure && state_->canvas->tool() != EditorTool::None && state_->canvas->tool() != EditorTool::Annotation);
        setNotesVisible(!pure && !state_->notesCollapsed && state_->notesModel->rowCount() > 0);
        if (pure) resize(state_->canvas->sizeHint().boundedTo(screen()->availableGeometry().size()));
        else if (state_->editExtent.isValid()) resize(state_->editExtent);
    });
    connect(state_->canvas, &EditorCanvas::viewChanged, this, [this](qreal previous, qreal current, QPointF anchor) {
        auto* horizontal = state_->scroll->horizontalScrollBar(); auto* vertical = state_->scroll->verticalScrollBar();
        const auto factor = current / previous;
        // The anchor is in canvas coordinates, already including scroll offset.
        horizontal->setValue(qRound(horizontal->value() + anchor.x() * (factor - 1)));
        vertical->setValue(qRound(vertical->value() + anchor.y() * (factor - 1)));
        state_->zoomLabel->setText(QString::number(qRound(current * 100)) + QChar(u'%'));
        state_->zoomLabel->adjustSize();
        state_->zoomLabel->move((std::max)(0, state_->scroll->viewport()->width() - state_->zoomLabel->width() - 8), 8);
        state_->zoomLabel->show(); state_->zoomLabel->raise(); state_->zoomHide->start();
    });
    connect(state_->canvas, &EditorCanvas::outputRequested, this, [this](OutputIntent intent) { requestOutput(intent); });
    connect(state_->canvas, &EditorCanvas::closeRequested, this, &QWidget::close);
    connect(state_->canvas, &EditorCanvas::operationFailed, this, [this](EditorFailure failure) { showFailure(failure); });
    connect(state_->notes->list, &QListView::clicked, this, [this](const QModelIndex& index) {
        state_->canvas->selectAnnotation(index.data(::Qt::UserRole).toULongLong(), true);
    });
    connect(state_->notes->list->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current) {
            if (!state_->changingNote && current.isValid()) state_->canvas->selectAnnotation(current.data(::Qt::UserRole).toULongLong(), false);
        });
    connect(state_->notes->editor, &QPlainTextEdit::textChanged, this, [this] {
        if (!state_->changingNote && state_->selectedNote)
            state_->canvas->setAnnotationNote(state_->selectedNote, state_->notes->editor->toPlainText());
    });
    connect(state_->notes->remove, &QToolButton::clicked, this, [this] { state_->canvas->deleteAnnotation(state_->selectedNote); });
    connect(state_->notes->collapse, &QToolButton::clicked, this, [this] { state_->notesCollapsed = true; setNotesVisible(false); });
    refreshLanguage(); updateActions();
}

EditorWindow::~EditorWindow()
{
    state_->notes->editor->removeEventFilter(this);
    state_->notes->list->removeEventFilter(this);
    disconnect(state_->canvas, nullptr, this, nullptr);
    state_->canvas->cancelPointerGesture(); state_->canvas->finishTextEditing();
}
EditorCanvas* EditorWindow::canvas() const noexcept { return state_->canvas; }
bool EditorWindow::iconFontAvailable() const noexcept { return !state_->iconFamily.isEmpty(); }
bool EditorWindow::outputPending() const noexcept { return state_->pendingToken != 0; }
void EditorWindow::setCloseOnSuccessfulOutput(bool close) noexcept { state_->closeAfterOutput = close; }
bool EditorWindow::outputEnabled(OutputIntent intent) const noexcept
{ return index(intent) < state_->outputs.size() && state_->outputs[index(intent)]; }
void EditorWindow::setOutputEnabled(OutputIntent intent, bool enabled)
{
    if (index(intent) >= state_->outputs.size()) return;
    state_->outputs[index(intent)] = enabled; updateActions();
}

EditorFailure EditorWindow::loadImage(const QImage& image)
{
    auto result = state_->canvas->loadImage(image);
    if (!result) { fitToWorkArea(false); updateActions(); }
    return result;
}

void EditorWindow::setTranslator(EditorTranslator translator)
{
    state_->translator = std::move(translator);
    state_->canvas->setTranslator(state_->translator);
    refreshLanguage();
}
QString EditorWindow::translated(QStringView key) const { return state_->translator ? state_->translator(key) : key.toString(); }

void EditorWindow::refreshLanguage()
{
    setWindowTitle(translated(u"cap.mark"));
    for (auto* button : findChildren<QToolButton*>()) {
        const auto key = button->property("languageKey").toString();
        if (key.isEmpty()) continue;
        const auto value = translated(key);
        button->setToolTip(value); button->setAccessibleName(value);
        if (button->icon().isNull()) button->setText(value);
    }
    state_->main->refreshTextWidths(); state_->style->refreshTextWidths();
    state_->notes->title->setText(translated(u"annotation.title"));
    state_->notes->collapse->setText(translated(u"annotation.collapse"));
    state_->notes->remove->setText(translated(u"annotation.remove"));
    state_->notes->editor->setPlaceholderText(translated(u"annotation.placeholder"));
    state_->notes->footer->setText(translated(u"annotation.saveHint"));
    state_->notes->footer->setToolTip(state_->notes->footer->text());
    state_->notesModel->setEmptyText(translated(u"annotation.empty"));
    state_->notes->heading->setText(translated(u"annotation.edit") + (state_->selectedNote ? QStringLiteral(" #") + QString::number(state_->selectedNote) : QString()));
    state_->notes->relayout();
}

void EditorWindow::makeMainToolbar()
{
    constexpr std::array<char16_t, 9> glyphs{u'\ue8e8', u'\ue6bc', u'\ue603', u'\ue776', 0, u'\ue601', u'\ue6ec', u'\ue82e', u'\ue6be'};
    for (std::size_t i = 0; i < editorTools.size(); ++i) {
        const auto tool = editorTools[i];
        auto* button = makeButton(state_->main, toolLanguageKey(tool), glyphs[i], state_->iconFamily, tool == EditorTool::Annotation);
        button->setCheckable(true); button->setProperty("tool", static_cast<int>(tool));
        state_->mainButtons.push_back(button); state_->main->add(button);
        connect(button, &QToolButton::clicked, this, [this, tool](bool checked) {
            state_->canvas->setTool(checked ? tool : EditorTool::None);
        });
    }
    state_->main->add(new ToolbarDivider(state_->main), static_cast<int>(UI::toolbarDivider));
    const auto action = [this](QString key, char16_t glyph) {
        auto* button = makeButton(state_->main, key, glyph, state_->iconFamily);
        state_->mainButtons.push_back(button); state_->main->add(button); return button;
    };
    state_->undo = action(QStringLiteral("tool.undo"), u'\ued85');
    state_->redo = action(QStringLiteral("tool.redo"), u'\ued8a');
    state_->main->add(new ToolbarDivider(state_->main), static_cast<int>(UI::toolbarDivider));
    state_->close = action(QStringLiteral("tool.close"), u'\ue62d');
    state_->save = action(QStringLiteral("tool.save"), u'\ue608');
    state_->clipboard = action(QStringLiteral("tool.clipboard"), u'\ue6ad');
    connect(state_->undo, &QToolButton::clicked, state_->canvas, &EditorCanvas::undo);
    connect(state_->redo, &QToolButton::clicked, state_->canvas, &EditorCanvas::redo);
    connect(state_->close, &QToolButton::clicked, this, &QWidget::close);
    connect(state_->save, &QToolButton::clicked, this, [this] { requestOutput(OutputIntent::SaveAs); });
    connect(state_->clipboard, &QToolButton::clicked, this, [this] { requestOutput(OutputIntent::Clipboard); });
}

void EditorWindow::rebuildStyleToolbar()
{
    state_->style->clear(); state_->toggles.clear(); state_->colors.clear(); state_->slider = nullptr;
    const auto tool = state_->canvas->tool();
    const auto* range = Editor::findToolSlider(toolId(tool));
    if (!range) { state_->style->hide(); return; }
    const auto toggle = [this, tool](QString key, char16_t glyph, const char* property) {
        auto* button = makeButton(state_->style, key, glyph, state_->iconFamily);
        button->setCheckable(true); button->setProperty("styleProperty", QByteArray(property));
        state_->toggles.push_back(button); state_->style->add(button);
        connect(button, &QToolButton::clicked, this, [this, tool, property](bool checked) {
            auto style = state_->canvas->toolStyle(tool);
            const QByteArray name(property);
            if (name == "filled") style.filled = checked;
            else if (name == "semiTransparent") style.semiTransparent = checked;
            else if (name == "bold") style.bold = checked;
            else if (name == "italic") style.italic = checked;
            else if (name == "rectangleMode") style.rectangleMode = checked;
            state_->canvas->setToolStyle(tool, style);
        });
    };
    switch (tool) {
    case EditorTool::Rectangle: toggle(QStringLiteral("tool.rectFill"), u'\ue602', "filled"); break;
    case EditorTool::Ellipse: toggle(QStringLiteral("tool.ellipseFill"), u'\ue600', "filled"); break;
    case EditorTool::Arrow: toggle(QStringLiteral("tool.arrowFill"), u'\ue604', "filled"); break;
    case EditorTool::Number: toggle(QStringLiteral("tool.numberFill"), u'\ue605', "filled"); break;
    case EditorTool::Line: toggle(QStringLiteral("tool.semiTransparent"), u'\ue607', "semiTransparent"); break;
    case EditorTool::Text:
        toggle(QStringLiteral("tool.bold"), u'\ue634', "bold"); toggle(QStringLiteral("tool.italic"), u'\ue682', "italic"); break;
    case EditorTool::Mosaic: case EditorTool::Eraser: toggle(QStringLiteral("tool.rectFill"), u'\ue602', "rectangleMode"); break;
    default: break;
    }
    auto* sliderBox = new QWidget(state_->style);
    auto* sliderLayout = new QHBoxLayout(sliderBox); sliderLayout->setContentsMargins(3, 0, 3, 0);
    state_->slider = new QSlider(::Qt::Horizontal, sliderBox); state_->slider->setObjectName(QStringLiteral("toolValue"));
    state_->slider->setFocusPolicy(::Qt::NoFocus); state_->slider->setRange(qRound(range->min), qRound(range->max));
    state_->slider->setSingleStep(1); state_->slider->setPageStep(1); sliderLayout->addWidget(state_->slider);
    state_->style->add(sliderBox, 106);
    connect(state_->slider, &QSlider::valueChanged, this, [this, tool](int value) {
        auto style = state_->canvas->toolStyle(tool); style.value = static_cast<float>(value);
        state_->canvas->setToolStyle(tool, style);
    });
    if (tool != EditorTool::Mosaic && tool != EditorTool::Eraser) {
        constexpr std::array<const char16_t*, 9> names{u"color.red",u"color.yellow",u"color.green",u"color.cyan",u"color.blue",u"color.purple",u"color.pink",u"color.black",u"color.white"};
        for (std::size_t i = 0; i < Editor::toolColors.size(); ++i) {
            auto* button = makeButton(state_->style, QString::fromUtf16(names[i]), 0, {});
            button->setIcon(QIcon(new EditorIcon(editorColor(Editor::toolColors[i]))));
            button->setToolButtonStyle(::Qt::ToolButtonIconOnly); button->setCheckable(true);
            state_->colors.push_back(button); state_->style->add(button);
            connect(button, &QToolButton::clicked, this, [this, tool, i] {
                auto style = state_->canvas->toolStyle(tool); style.rgba = Editor::toolColors[i];
                state_->canvas->setToolStyle(tool, style); updateStyleToolbar();
            });
        }
    }
    updateStyleToolbar(); refreshLanguage(); state_->style->setVisible(!state_->canvas->pureImageMode());
}

void EditorWindow::updateStyleToolbar()
{
    const auto style = state_->canvas->toolStyle(state_->canvas->tool());
    if (state_->slider) {
        const QSignalBlocker blocked(state_->slider); state_->slider->setValue(qRound(style.value));
        state_->slider->setToolTip(QString::number(style.value));
    }
    for (auto* button : state_->toggles) {
        const auto name = button->property("styleProperty").toByteArray();
        button->setChecked(name == "filled" ? style.filled : name == "semiTransparent" ? style.semiTransparent :
            name == "bold" ? style.bold : name == "italic" ? style.italic : style.rectangleMode);
    }
    for (std::size_t i = 0; i < state_->colors.size(); ++i) state_->colors[i]->setChecked(Editor::toolColors[i] == style.rgba);
}

void EditorWindow::updateActions()
{
    const bool active = !outputPending() && state_->canvas->hasImage();
    for (auto* button : state_->mainButtons) button->setEnabled(active);
    state_->close->setEnabled(!outputPending());
    state_->undo->setEnabled(active && state_->canvas->canUndo()); state_->redo->setEnabled(active && state_->canvas->canRedo());
    state_->save->setEnabled(active && outputEnabled(OutputIntent::SaveAs));
    state_->clipboard->setEnabled(active && outputEnabled(OutputIntent::Clipboard));
    state_->style->setEnabled(active); state_->notes->setEnabled(active);
}

void EditorWindow::syncAnnotations()
{
    const auto notes = state_->canvas->document().annotationNotes();
    state_->changingNote = true; state_->notesModel->setNotes(notes); state_->changingNote = false;
    if (state_->canvas->pointerGestureActive()) { state_->deferredNotes = true; return; }
    if (!state_->canvas->pureImageMode() && !state_->notesCollapsed && !notes.empty()) setNotesVisible(true);
    selectNote(state_->canvas->document().selectedAnnotation(), false);
}

void EditorWindow::selectNote(quint64 number, bool focus)
{
    auto* shape = state_->canvas->document().findAnnotation(number);
    if (!shape) number = 0;
    const bool changed = number != state_->selectedNote;
    if (changed) QGuiApplication::inputMethod()->commit();
    state_->selectedNote = number;
    state_->changingNote = true;
    const auto modelIndex = state_->notesModel->find(number);
    state_->notes->list->setCurrentIndex(modelIndex);
    if (modelIndex.isValid()) state_->notes->list->scrollTo(modelIndex, QAbstractItemView::EnsureVisible);
    if (changed || (!number && !state_->notes->editor->toPlainText().isEmpty())) {
        state_->notes->editor->setPlainText(shape ? QString::fromStdWString(shape->annotationModel()->note()) : QString());
    }
    state_->changingNote = false;
    state_->notes->editor->setEnabled(number != 0 && !outputPending());
    state_->notes->remove->setEnabled(number != 0 && !outputPending());
    state_->notes->heading->setText(translated(u"annotation.edit") + (number ? QStringLiteral(" #") + QString::number(number) : QString()));
    if (focus && number && !state_->canvas->pointerGestureActive()) {
        state_->notesCollapsed = false; setNotesVisible(true);
        state_->notes->editor->setFocus(::Qt::OtherFocusReason);
        if (changed) { auto cursor = state_->notes->editor->textCursor(); cursor.movePosition(QTextCursor::End); state_->notes->editor->setTextCursor(cursor); }
    }
}

void EditorWindow::setNotesVisible(bool visible)
{
    if (state_->canvas->pointerGestureActive()) { state_->deferredNotes = true; return; }
    if (visible == !state_->notes->isHidden()) return;
    if (!visible) QGuiApplication::inputMethod()->commit();
    state_->notes->setVisible(visible); state_->canvas->setAnnotationPanelVisible(visible);
    if (visible) fitToWorkArea(true);
}

void EditorWindow::fitToWorkArea(bool includeNotes)
{
    if (!state_->canvas->hasImage()) return;
    const auto area = screen()->availableGeometry();
    const auto available = area.size() - QSize(16, 100);
    const int panelWidth = includeNotes ? std::clamp(qRound(available.width() * .36), 240, 340) : 0;
    if (includeNotes) state_->notes->setFixedWidth((std::min)(panelWidth, (std::max)(160, available.width() - 80)));
    const int imageWidth = (std::max)(20, available.width() - (includeNotes ? state_->notes->width() + 10 : 0));
    const int imageHeight = (std::max)(60, available.height() - 100);
    const auto size = state_->canvas->originalImage().size();
    const auto dpr = state_->canvas->devicePixelRatioF();
    const auto fit = (std::min)(imageWidth * dpr / size.width(), imageHeight * dpr / size.height());
    state_->canvas->setZoom((std::min)(state_->canvas->zoom(), fit));
    const auto canvasSize = state_->canvas->sizeHint();
    const int width = std::clamp(canvasSize.width() + (includeNotes ? state_->notes->width() + 10 : 0),
        (std::min)(320, (std::max)(46, available.width())), (std::max)(320, available.width()));
    const int chrome = state_->main->heightForWidth(width) + state_->style->heightForWidth(width) + 4;
    resize(width, (std::min)((std::max)(includeNotes ? 240 : 1, canvasSize.height()) + chrome,
        (std::max)(240, area.height() - 16)));
    // Position is owned by the host/native move loop; refresh and DPI changes
    // never recenter the window or pull it back onto a previous monitor.
}

void EditorWindow::showFailure(EditorFailure failure, const QString& message)
{
    state_->status->setText(message.isEmpty() ? translated(u"annotation.exportFailed") : message);
    state_->status->show(); emit operationFailed(failure);
}

void EditorWindow::setBusy(bool busy)
{
    state_->canvas->setInteractionEnabled(!busy); updateActions(); emit outputPendingChanged(busy);
}

bool EditorWindow::requestOutput(OutputIntent intent)
{
    if (outputPending() || !outputEnabled(intent) || !state_->canvas->hasImage() || state_->canvas->pointerGestureActive()) return false;
    if (state_->nextToken == 0) { showFailure({EditorFailure::Code::InvalidOperation}); return false; }
    QGuiApplication::inputMethod()->commit();
    QImage image;
    const auto error = state_->canvas->renderExport(image);
    if (error) { showFailure(error); return false; }
    state_->previousFocus = QApplication::focusWidget();
    state_->pendingToken = state_->nextToken++;
    state_->pendingIntent = intent;
    state_->status->hide();
    const auto token = state_->pendingToken;
    const QPointer<EditorWindow> lifetime(this);
    setBusy(true);
    if (!lifetime) return true;
    // A queued Qt connection owns an implicit-shared QImage snapshot. Synchronous
    // completion may close the window, so do not access members after emitting.
    emit outputRequested(token, intent, image);
    return true;
}

void EditorWindow::completeOutput(quint64 token, OutputResult result, const QString& message)
{
    if (!token || token != state_->pendingToken) return;
    const auto intent = state_->pendingIntent;
    const auto focus = state_->previousFocus;
    const bool close = result == OutputResult::Succeeded && state_->closeAfterOutput;
    state_->pendingToken = 0;
    const QPointer<EditorWindow> lifetime(this);
    setBusy(false);
    if (!lifetime) return;
    if (result == OutputResult::Failed) {
        const auto fallback = intent == OutputIntent::Clipboard ? translated(u"clipboard.copyFailed") : translated(u"clipboard.saveFailed");
        showFailure({EditorFailure::Code::InvalidOperation}, message.isEmpty() ? fallback : message);
        if (!lifetime) return;
    }
    if (focus && !close) focus->setFocus(::Qt::OtherFocusReason);
    emit outputFinished(intent, result);
    if (lifetime && close) this->close();
}

bool EditorWindow::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == state_->notes->editor || watched == state_->notes->list) && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers().testFlag(::Qt::ControlModifier) && key->key() == ::Qt::Key_S) {
            requestOutput(OutputIntent::SaveAs); return true;
        }
        if (watched == state_->notes->list && key->key() == ::Qt::Key_Delete) {
            state_->canvas->deleteAnnotation(state_->selectedNote); return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void EditorWindow::closeEvent(QCloseEvent* event)
{
    if (outputPending()) { event->ignore(); emit outputCancellationRequested(state_->pendingToken); return; }
    state_->canvas->cancelPointerGesture(); state_->canvas->finishTextEditing();
    QGuiApplication::inputMethod()->commit();
    event->accept();
}
void EditorWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::WindowDeactivate) state_->canvas->cancelPointerGesture();
    if (event->type() == QEvent::LanguageChange) refreshLanguage();
    QWidget::changeEvent(event);
}

} // namespace HuhuCapture::UI::Qt
