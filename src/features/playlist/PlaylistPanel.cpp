// CGPlay PlaylistPanel.cpp — Phase 3: Shot Cards
// Each card: thumbnail (120x68) + Name + Resolution + FPS + Duration + Status

#include "PlaylistPanel.h"
#include "PlaylistModel.h"
#include "playback/api/IPlaybackService.h"
#include "annotation/ReviewExport.h"
#include "media/MediaProbe.h"
#include "otio/OtioImporter.h"
#include "otio/OtioExporter.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListView>
#include <QLineEdit>
#include <QToolButton>
#include <QLabel>
#include <QSortFilterProxyModel>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QKeyEvent>
#include <QMenu>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QPainterPath>
#include <QMessageBox>
#include <QMainWindow>
#include <QStatusBar>
#include <QComboBox>
#include <QApplication>
#include <QPalette>
#include <QPaintEvent>
#include <QPixmap>
#include <cmath>

namespace cgplay {

// ─── Colors ────────────────────────────────────────────────────────────────────
static const QColor kBg       = QColor(15, 20, 26, 210);
static const QColor kGlass    = QColor(18, 24, 31, 168);
static const QColor kCard     = QColor(17, 22, 28, 188);
static const QColor kBorder   = QColor(255, 255, 255, 18);
static const QColor kHiBorder = QColor(255, 255, 255, 30);
static const QColor kText     = QColor(0xD8,0xDE,0xE7);
static const QColor kSec      = QColor(0x9A,0xA4,0xB2);
static const QColor kApproved = QColor(0x2E,0xCC,0x71);
static const QColor kReview   = QColor(0xF1,0xC4,0x0F);
static const QColor kNeedsFix = QColor(0xE7,0x4C,0x3C);
static const QColor kWIP      = QColor(0x34,0x98,0xDB);

static QColor themeColor(const char* propertyName, const QColor& fallback)
{
    if (!qApp) return fallback;
    const QColor value(qApp->property(propertyName).toString());
    return value.isValid() ? value : fallback;
}

static QColor surfaceColor(const char* propertyName, const QColor& fallback, const char* opacityProperty, int fallbackOpacity)
{
    QColor color = themeColor(propertyName, fallback);
    const int opacity = qApp ? qBound(0, qApp->property(opacityProperty).toInt(), 100) : fallbackOpacity;
    color.setAlpha(qRound(opacity * 255.0 / 100.0));
    return color;
}

static bool lightTheme()
{
    return qApp && qApp->property("cgplay.themeMode").toString().compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0;
}

// ─── ShotCardDelegate ──────────────────────────────────────────────────────────
class ShotCardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);

        const bool light = lightTheme();
        const QColor panel = themeColor(
            "cgplay.panelColor",
            QApplication::palette().color(QPalette::Base));
        const QColor text = themeColor(
            "cgplay.textColor",
            QApplication::palette().color(QPalette::Text));
        const QColor secondaryText = QApplication::palette().color(QPalette::Disabled, QPalette::Text);
        const QColor accent = themeColor(
            "cgplay.accentColor",
            QApplication::palette().color(QPalette::Highlight));
        const QColor border = themeColor(
            "cgplay.borderColor",
            light ? QColor(0xB8, 0xC2, 0xCE) : QColor(255, 255, 255, 30));

        QRect r = opt.rect.adjusted(8, 4, -8, -4);
        bool sel = opt.state & QStyle::State_Selected;
        bool hov = opt.state & QStyle::State_MouseOver;

        // ── Card background ──────────────────────────────────────────────
        QPainterPath cardPath;
        cardPath.addRoundedRect(QRectF(r), 7, 7);
        QLinearGradient cardGrad(r.topLeft(), r.bottomLeft());
        if (sel) {
            cardGrad.setColorAt(0.0, accent.lighter(light ? 112 : 118));
            cardGrad.setColorAt(1.0, panel);
        } else if (hov) {
            cardGrad.setColorAt(0.0, panel.lighter(light ? 108 : 112));
            cardGrad.setColorAt(1.0, panel.darker(light ? 102 : 108));
        } else {
            cardGrad.setColorAt(0.0, panel);
            cardGrad.setColorAt(1.0, panel.darker(light ? 104 : 112));
        }
        const QColor edge = sel ? accent : border;
        p->setPen(QPen(edge, 1));
        p->setBrush(cardGrad);
        p->drawPath(cardPath);
        p->setPen(QPen(QColor(text.red(), text.green(), text.blue(), sel ? 48 : 18), 1));
        p->drawLine(r.left() + 8, r.top() + 1, r.right() - 8, r.top() + 1);
        if (sel) {
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(accent.red(), accent.green(), accent.blue(), light ? 34 : 52));
            p->drawRoundedRect(QRect(r.left() + 1, r.top() + 1, 3, r.height() - 2), 2, 2);
        }

        int thumbW = 116, thumbH = 64;

        // ── Thumbnail ────────────────────────────────────────────────────
        QRect thumbRect(r.x()+8, r.y()+8, thumbW, thumbH);
        p->setPen(QPen(QColor(255, 255, 255, 24), 1));
        const int row = idx.row();
        QPainterPath thumbPath;
        thumbPath.addRoundedRect(thumbRect, 5, 5);
        p->save();
        p->setClipPath(thumbPath);
        const QIcon thumbIcon = qvariant_cast<QIcon>(idx.data(Qt::DecorationRole));
        if (!thumbIcon.isNull()) {
            const QPixmap pm = thumbIcon.pixmap(thumbRect.size());
            p->drawPixmap(thumbRect, pm);
        } else {
            QLinearGradient thumbGrad(thumbRect.topLeft(), thumbRect.bottomRight());
            thumbGrad.setColorAt(0.0, panel.lighter(light ? 106 : 112));
            thumbGrad.setColorAt(0.58, panel);
            thumbGrad.setColorAt(1.0, panel.darker(light ? 110 : 125));
            p->fillRect(thumbRect, thumbGrad);
            p->setPen(QColor(secondaryText.red(), secondaryText.green(), secondaryText.blue(), 180));
            QFont emptyFont("Segoe UI", 8);
            emptyFont.setWeight(QFont::DemiBold);
            p->setFont(emptyFont);
            p->drawText(thumbRect, Qt::AlignCenter, QStringLiteral("NO THUMB"));
        }
        p->fillRect(thumbRect, QColor(0, 0, 0, 4));
        p->restore();
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(thumbRect, 5, 5);

        int tx = r.x() + thumbW + 20;

        // ── Shot name ────────────────────────────────────────────────────
        QFont nameFont("Segoe UI", 11);
        nameFont.setBold(true);
        p->setFont(nameFont);
        p->setPen(text);
        QString name = idx.data(PlaylistModel::NameRole).toString();
        p->drawText(QRect(tx, r.y()+7, r.right()-tx-8, 21), Qt::AlignLeft|Qt::AlignVCenter,
                    p->fontMetrics().elidedText(name, Qt::ElideRight, r.width()-tx-20));

        // ── Resolution | FPS | Duration ──────────────────────────────────
        QFont metaFont("Segoe UI", 8);
        p->setFont(metaFont);
        p->setPen(secondaryText);
        double fps = idx.data(Qt::UserRole+10).toDouble(); // we'll stash FPS here
        if (fps <= 0) fps = 24.0;
        int frames = idx.data(PlaylistModel::FrameCountRole).toInt();
        if (frames <= 0) frames = 1;
        QRect metaRect(tx, r.y()+29, r.right()-tx-8, 16);
        const int mediaW = idx.data(PlaylistModel::WidthRole).toInt();
        const int mediaH = idx.data(PlaylistModel::HeightRole).toInt();
        const QString resText = (mediaW > 0 && mediaH > 0)
            ? QString("%1x%2").arg(mediaW).arg(mediaH)
            : QStringLiteral("--");
        const int seconds = fps > 0.0 ? static_cast<int>(std::round(frames / fps)) : 0;
        p->drawText(metaRect, Qt::AlignLeft|Qt::AlignVCenter,
                    QString("%4  ·  %1 FPS  ·  %2f  ·  %3s")
                        .arg(fps, 0, 'f', 1)
                        .arg(frames)
                        .arg(seconds)
                        .arg(resText));

        // ── Format badge + Status ────────────────────────────────────────
        QString format = idx.data(PlaylistModel::FormatRole).toString();
        QRect badgeRect(tx, r.y()+49, 36, 15);
        QColor badgeBg = (format=="EXR") ? QColor(0x5A,0x82,0x32) :
                         (format=="DPX") ? QColor(0x82,0x5A,0x32) : QColor(0x32,0x5A,0x82);
        p->setPen(Qt::NoPen);
        p->setBrush(badgeBg);
        p->drawRoundedRect(badgeRect, 4, 4);
        p->setPen(Qt::white);
        QFont badgeFont("Segoe UI", 7);
        badgeFont.setBold(true);
        p->setFont(badgeFont);
        p->drawText(badgeRect, Qt::AlignCenter, format);

        // ── Status label ─────────────────────────────────────────────────
        QColor sc = idx.data(PlaylistModel::StatusColorRole).value<QColor>();
        if (!sc.isValid()) sc = kSec;
        QString statusText;
        if (sc == kApproved) statusText = QString::fromUtf8("已通过");
        else if (sc == kReview) statusText = QString::fromUtf8("审阅中");
        else if (sc == kNeedsFix) statusText = QString::fromUtf8("需修改");
        else if (sc == kWIP) statusText = "WIP";

        if (!statusText.isEmpty()) {
            QRect statusRect(tx+42, r.y()+49, 82, 15);
            p->setPen(sc);
            p->setFont(badgeFont);
            p->drawText(statusRect, Qt::AlignLeft|Qt::AlignVCenter, statusText);
            // Status dot
            p->setPen(Qt::NoPen);
            p->setBrush(sc);
            p->drawEllipse(QPointF(tx+37, r.y()+56), 3.5, 3.5);
        }

        // ── Bottom border ────────────────────────────────────────────────
        p->setPen(QPen(QColor(255,255,255,10), 1));
        p->drawLine(r.x()+10, r.bottom(), r.right()-10, r.bottom());

        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(300, 82);
    }
};

class PlaylistFilterProxyModel : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setStatusFilter(const QColor& color)
    {
        _statusColor = color;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        if (!idx.isValid()) return false;

        const QString text = filterRegularExpression().pattern();
        if (!text.isEmpty() &&
            !idx.data(PlaylistModel::NameRole).toString().contains(text, Qt::CaseInsensitive)) {
            return false;
        }

        if (_statusColor.isValid()) {
            return idx.data(PlaylistModel::StatusColorRole).value<QColor>() == _statusColor;
        }
        return true;
    }

private:
    QColor _statusColor;
};

// ─── Private ────────────────────────────────────────────────────────────────────
struct PlaylistPanel::Private
{
    std::shared_ptr<IPlaybackService> playback;
    PlaylistModel*          model       = nullptr;
    PlaylistFilterProxyModel* proxyModel = nullptr;
    QListView*              listView    = nullptr;
    QLineEdit*              searchEdit  = nullptr;
    QComboBox*              filterCombo = nullptr;
    int shotA = -1, shotB = -1;
};

// ─── Constructor ────────────────────────────────────────────────────────────────
PlaylistPanel::PlaylistPanel(std::shared_ptr<IPlaybackService> playback, QWidget* parent)
    : QWidget(parent), _p(std::make_unique<Private>())
{
    _p->playback = std::move(playback);
    setAcceptDrops(true);
    setMinimumWidth(0);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_StyledBackground, false);

    auto* vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(0,0,0,0);
    vlay->setSpacing(0);

    // ── Header ─────────────────────────────────────────────────────────────
    auto* header = new QWidget(this);
    header->setStyleSheet("background: transparent;");
    auto* hl = new QVBoxLayout(header);
    hl->setContentsMargins(12, 10, 12, 8);
    hl->setSpacing(8);

    // Title row
    auto* titleRow = new QHBoxLayout();
    auto* titleLbl = new QLabel(QString::fromUtf8("播放列表"), this);
    titleLbl->setObjectName(QStringLiteral("PlaylistTitle"));
    titleLbl->setStyleSheet(QStringLiteral("font-size:12px;font-weight:750;background:transparent;"));
    titleRow->addWidget(titleLbl);
    titleRow->addStretch();

    auto addBtn = [&](const QString& txt, const QString& tip, const QString& extra={}) -> QToolButton* {
        auto* b = new QToolButton(this);
        b->setText(txt); b->setToolTip(tip); b->setFixedSize(26,26);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("QToolButton{border-radius:6px;font-size:12px;}") + extra);
        return b;
    };
    auto* btnAdd = addBtn("+", QString::fromUtf8("添加文件"));
    auto* btnDel = addBtn(QString::fromUtf8("−"), QString::fromUtf8("移除"));
    auto* btnClr = addBtn(QString::fromUtf8("✕"), QString::fromUtf8("清空"));
    auto* btnA   = addBtn("A", QString::fromUtf8("设为对比 A"), "QToolButton{color:#2ECC71;font-weight:700;}");
    auto* btnB   = addBtn("B", QString::fromUtf8("设为对比 B"), "QToolButton{color:#E74C3C;font-weight:700;}");
    auto* btnSt  = addBtn(QString::fromUtf8("●"), QString::fromUtf8("切换状态"));
    titleRow->addWidget(btnAdd); titleRow->addWidget(btnDel); titleRow->addWidget(btnClr);
    titleRow->addWidget(btnA); titleRow->addWidget(btnB); titleRow->addWidget(btnSt);
    hl->addLayout(titleRow);

    // ── Search + Filter ────────────────────────────────────────────────────
    auto* sfRow = new QHBoxLayout();
    sfRow->setSpacing(8);
    _p->searchEdit = new QLineEdit(this);
    _p->searchEdit->setPlaceholderText(QString::fromUtf8("搜索片段..."));
    _p->searchEdit->setFixedHeight(32);
    _p->searchEdit->setObjectName(QStringLiteral("PlaylistSearch"));
    _p->searchEdit->setStyleSheet(QStringLiteral("QLineEdit{border-radius:7px;padding:5px 10px;font-size:12px;}"));
    _p->searchEdit->setClearButtonEnabled(true);
    sfRow->addWidget(_p->searchEdit, 1);

    _p->filterCombo = new QComboBox(this);
    _p->filterCombo->addItems({QString::fromUtf8("全部状态"), QString::fromUtf8("已通过"), QString::fromUtf8("审阅中"), QString::fromUtf8("需修改"), "WIP"});
    _p->filterCombo->setFixedHeight(32);
    _p->filterCombo->setObjectName(QStringLiteral("PlaylistFilter"));
    _p->filterCombo->setStyleSheet(QStringLiteral("QComboBox{border-radius:7px;padding:4px 8px;font-size:11px;}"));
    sfRow->addWidget(_p->filterCombo);
    hl->addLayout(sfRow);
    vlay->addWidget(header);

    // ── List view ───────────────────────────────────────────────────────────
    _p->model = new PlaylistModel(this);
    _p->proxyModel = new PlaylistFilterProxyModel(this);
    _p->proxyModel->setSourceModel(_p->model);
    _p->proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

    _p->listView = new QListView(this);
    _p->listView->setModel(_p->proxyModel);
    _p->listView->setItemDelegate(new ShotCardDelegate(_p->listView));
    _p->listView->setDragDropMode(QAbstractItemView::DragDrop);
    _p->listView->setDefaultDropAction(Qt::CopyAction);
    _p->listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _p->listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _p->listView->setSpacing(0);
    _p->listView->setStyleSheet("QListView{background:transparent;border:none;outline:none;padding:2px 0 10px 0;}");
    _p->listView->setContextMenuPolicy(Qt::CustomContextMenu);
    vlay->addWidget(_p->listView, 1);

    // ── Connections ─────────────────────────────────────────────────────────
    connect(btnAdd, &QToolButton::clicked, this, &PlaylistPanel::_onAddFiles);
    connect(btnDel, &QToolButton::clicked, this, &PlaylistPanel::_onRemove);
    connect(btnClr, &QToolButton::clicked, this, &PlaylistPanel::_onClear);
    connect(btnA, &QToolButton::clicked, this, &PlaylistPanel::_onSetShotA);
    connect(btnB, &QToolButton::clicked, this, &PlaylistPanel::_onSetShotB);
    connect(btnSt, &QToolButton::clicked, this, &PlaylistPanel::_onCycleStatus);
    connect(_p->searchEdit, &QLineEdit::textChanged, this, &PlaylistPanel::_onSearch);
    connect(_p->filterCombo, &QComboBox::currentTextChanged, this, [this](const QString& sel){
        QColor color;
        if (sel == QString::fromUtf8("已通过")) color = kApproved;
        else if (sel == QString::fromUtf8("审阅中")) color = kReview;
        else if (sel == QString::fromUtf8("需修改")) color = kNeedsFix;
        else if (sel == "WIP") color = kWIP;
        _p->proxyModel->setStatusFilter(color);
    });
    connect(_p->listView, &QListView::activated, this, &PlaylistPanel::_onItemActivated);
    connect(_p->listView, &QListView::doubleClicked, this, &PlaylistPanel::_onItemActivated);
    connect(_p->listView, &QListView::customContextMenuRequested, this, [this](const QPoint& pos){
        QMenu m(this);
        m.setObjectName(QStringLiteral("PlaylistContextMenu"));
        m.addAction(QString::fromUtf8("设为 Shot A"), this, &PlaylistPanel::_onSetShotA);
        m.addAction(QString::fromUtf8("设为 Shot B"), this, &PlaylistPanel::_onSetShotB);
        m.addSeparator();
        m.addAction(QString::fromUtf8("切换状态"), this, &PlaylistPanel::_onCycleStatus);
        m.addSeparator();
        m.addAction(QString::fromUtf8("移除"), this, &PlaylistPanel::_onRemove);
        m.addAction(QString::fromUtf8("清空"), this, &PlaylistPanel::_onClear);
        m.exec(_p->listView->mapToGlobal(pos));
    });
}

PlaylistPanel::~PlaylistPanel() = default;
PlaylistModel* PlaylistPanel::model() const { return _p->model; }

void PlaylistPanel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool light = lightTheme();
    const QColor panel = surfaceColor("cgplay.panelColor", QColor(kBg), "cgplay.panelOpacity", 96);
    const QColor secondary = surfaceColor("cgplay.backgroundSecondary", panel.darker(112), "cgplay.panelOpacity", 96);
    const QColor border = themeColor("cgplay.borderColor", QColor(255, 255, 255, 24));
    const QColor accent = themeColor("cgplay.accentColor", QColor(0xFF, 0x8A, 0x3D));

    const QRectF panelRect = rect().adjusted(0, 0, -1, -1);
    QPainterPath panelPath;
    panelPath.addRoundedRect(panelRect, 8, 8);
    painter.setClipPath(panelPath);

    QLinearGradient glass(0, 0, 0, height());
    glass.setColorAt(0.0, panel.lighter(light ? 104 : 108));
    glass.setColorAt(0.42, panel);
    glass.setColorAt(1.0, secondary);
    painter.fillPath(panelPath, glass);

    QLinearGradient topEdge(0, 0, 0, 46);
    topEdge.setColorAt(0.0, light ? QColor(255, 255, 255, 32) : QColor(255, 255, 255, 13));
    topEdge.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.fillRect(QRect(0, 0, width(), 46), topEdge);

    if (!light) {
        QRadialGradient warm(width() * 0.14, height() * 0.08, width() * 0.58);
        warm.setColorAt(0.0, QColor(accent.red(), accent.green(), accent.blue(), 12));
        warm.setColorAt(0.52, QColor(accent.red(), accent.green(), accent.blue(), 3));
        warm.setColorAt(1.0, QColor(0, 0, 0, 0));
        painter.fillRect(rect(), warm);
    }

    painter.setClipping(false);
    painter.setPen(QPen(light ? border : QColor(255, 255, 255, 17), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(panelPath);
    painter.setPen(QPen(light ? border.darker(115) : QColor(0, 0, 0, 72), 1));
    painter.drawLine(width() - 1, 10, width() - 1, height() - 10);
}

int PlaylistPanel::currentShotIndex() const
{
    auto sel = _p->listView->selectionModel()->selectedIndexes();
    if (sel.isEmpty()) return -1;
    return _p->proxyModel->mapToSource(sel.first()).row();
}

void PlaylistPanel::_onAddFiles()
{
    QStringList paths = QFileDialog::getOpenFileNames(this, QString::fromUtf8("添加文件"), {},
        MediaProbe::mediaFileDialogFilter());
    for (auto& p : paths) {
        const QString suffix = QFileInfo(p).suffix().toLower();
        if (ReviewExport::isStillImage(p) && (suffix == "exr" || suffix == "dpx")) {
            Q_EMIT sequenceImportRequested(p);
        } else {
            _p->model->addPath(p);
        }
    }
}
void PlaylistPanel::_onRemove()
{
    auto sel = _p->listView->selectionModel()->selectedIndexes();
    if (sel.isEmpty()) {
        return;
    }
    std::vector<int> rows;
    QStringList removedPaths;
    for (auto& i : sel) {
        const int row = _p->proxyModel->mapToSource(i).row();
        if (row >= 0 && std::find(rows.begin(), rows.end(), row) == rows.end()) {
            rows.push_back(row);
            if (row < _p->model->shotCount()) {
                removedPaths << _p->model->shotAt(row).path;
            }
        }
    }
    std::sort(rows.rbegin(), rows.rend());
    for (int r : rows) {
        _p->model->removeShotAt(r);
    }
    if (!removedPaths.isEmpty()) {
        Q_EMIT shotsRemoved(removedPaths);
    }
}
void PlaylistPanel::_onClear()
{
    QStringList removedPaths;
    for (int i = 0; i < _p->model->shotCount(); ++i) {
        removedPaths << _p->model->shotAt(i).path;
    }
    _p->model->clear();
    _p->shotA = _p->shotB = -1;
    if (!removedPaths.isEmpty()) {
        Q_EMIT playlistCleared(removedPaths);
    }
}
void PlaylistPanel::_onSearch(const QString& t) { _p->proxyModel->setFilterFixedString(t); }
void PlaylistPanel::_onItemActivated(const QModelIndex& i)
{
    QModelIndex src = _p->proxyModel->mapToSource(i);
    if (!src.isValid()) return;
    const auto& s = _p->model->shotAt(src.row());
    _p->model->setCurrentIndex(src.row());
    Q_EMIT shotActivated(s.path);
}
void PlaylistPanel::_onSetShotA() { int i=currentShotIndex(); if(i>=0){_p->shotA=i; const auto& s=_p->model->shotAt(i); Q_EMIT shotASelected(s.path);} }
void PlaylistPanel::_onSetShotB() { int i=currentShotIndex(); if(i>=0){_p->shotB=i; const auto& s=_p->model->shotAt(i); Q_EMIT shotBSelected(s.path);} }
void PlaylistPanel::_onCycleStatus()
{
    auto sel = _p->listView->selectionModel()->selectedIndexes();
    if (sel.isEmpty()) return;
    static const QColor colors[] = {kSec, kApproved, kReview, kNeedsFix, kWIP};
    for (auto& i : sel) {
        int r = _p->proxyModel->mapToSource(i).row();
        QColor cur = _p->model->statusColorAt(r);
        int ci=0; for (int j=1;j<5;j++) if(cur==colors[j]){ci=j;break;}
        QColor next = colors[(ci+1)%5];
        _p->model->setStatusColor(r, next);
        Q_EMIT statusChanged(r, next);
    }
}

void PlaylistPanel::_onSortAsc() { _p->model->sort(true); }
void PlaylistPanel::_onSortDesc() { _p->model->sort(false); }

void PlaylistPanel::keyPressEvent(QKeyEvent* e)
{
    switch(e->key()) {
    case Qt::Key_Up: case Qt::Key_Down: {
        auto sel = _p->listView->selectionModel()->selectedIndexes();
        int row = sel.isEmpty() ? 0 : sel.first().row() + (e->key()==Qt::Key_Up?-1:1);
        if (row<0) row=_p->proxyModel->rowCount()-1;
        if (row>=_p->proxyModel->rowCount()) row=0;
        QModelIndex n = _p->proxyModel->index(row,0);
        if (n.isValid()) { _p->listView->selectionModel()->select(n, QItemSelectionModel::ClearAndSelect); _p->listView->scrollTo(n); _onItemActivated(n); }
        return;
    }
    case Qt::Key_Enter: case Qt::Key_Return: {
        auto sel = _p->listView->selectionModel()->selectedIndexes();
        if (!sel.isEmpty()) _onItemActivated(sel.first());
        return;
    }
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        _onRemove();
        return;
    }
    QWidget::keyPressEvent(e);
}

void PlaylistPanel::dragEnterEvent(QDragEnterEvent* e) { if(e->mimeData()->hasUrls()) e->acceptProposedAction(); }
void PlaylistPanel::dropEvent(QDropEvent* e) {
    for (auto& u : e->mimeData()->urls()) {
        if (!u.isLocalFile()) {
            continue;
        }
        const QString path = u.toLocalFile();
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (ReviewExport::isStillImage(path) && (suffix == "exr" || suffix == "dpx")) {
            Q_EMIT sequenceImportRequested(path);
        } else {
            _p->model->addPath(path);
        }
    }
    e->acceptProposedAction();
}

void PlaylistPanel::_onImportOtio()
{
    QString path = QFileDialog::getOpenFileName(this, QString::fromUtf8("导入 OTIO"), {}, "OTIO (*.otio);;JSON (*.json)");
    if (path.isEmpty()) return;
    OtioImporter i;
    if (i.importToModel(path, _p->model)) {
        auto* mw = qobject_cast<QMainWindow*>(window());
        if (mw && mw->statusBar()) mw->statusBar()->showMessage(QString::fromUtf8("OTIO 已导入"), 3000);
    }
}
void PlaylistPanel::_onExportOtio()
{
    if (_p->model->rowCount()==0) return;
    QString path = QFileDialog::getSaveFileName(this, QString::fromUtf8("导出 OTIO"), "timeline.otio", "OTIO (*.otio)");
    if (path.isEmpty()) return;
    OtioExporter e;
    if (e.exportToFile(path, _p->model)) {
        auto* mw = qobject_cast<QMainWindow*>(window());
        if (mw && mw->statusBar()) mw->statusBar()->showMessage(QString::fromUtf8("OTIO 已导出"), 3000);
    }
}

} // namespace cgplay
