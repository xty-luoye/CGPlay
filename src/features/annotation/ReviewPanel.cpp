#include "ReviewPanel.h"

#include "AnnotationItem.h"
#include "AnnotationManager.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>

namespace cgplay {

static const char* kPanelBg = "rgba(15,20,26,0.76)";
static const char* kInputBg = "rgba(8,12,16,0.54)";
static const char* kBorder = "rgba(255,255,255,0.08)";
static const char* kText = "#D8DEE7";
static const char* kSec = "#9AA4B2";
static const char* kAccent = "#FF8A3D";

QColor runtimeColor(const char* propertyName, const QColor& fallback)
{
    if (!qApp) return fallback;
    const QColor value(qApp->property(propertyName).toString());
    return value.isValid() ? value : fallback;
}

static QColor surfaceColor(const char* propertyName, const QColor& fallback, const char* opacityProperty, int fallbackOpacity)
{
    QColor color = runtimeColor(propertyName, fallback);
    const int opacity = qApp ? qBound(0, qApp->property(opacityProperty).toInt(), 100) : fallbackOpacity;
    color.setAlpha(qRound(opacity * 255.0 / 100.0));
    return color;
}

namespace {

QString timecodeFromFrame(int frame, int fps = 24)
{
    fps = std::max(1, fps);
    const int ff = frame % fps;
    const int totalSeconds = frame / fps;
    const int ss = totalSeconds % 60;
    const int mm = (totalSeconds / 60) % 60;
    const int hh = totalSeconds / 3600;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hh, 2, 10, QChar('0'))
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}

QColor statusColor(ReviewStatus status)
{
    switch (status) {
    case ReviewStatus::Open:
        return QColor(0xF1, 0xC4, 0x0F);
    case ReviewStatus::InProgress:
        return QColor(0x34, 0x98, 0xDB);
    case ReviewStatus::Resolved:
        return QColor(0x2E, 0xCC, 0x71);
    default:
        return QColor(kSec);
    }
}

} // namespace

ReviewPanel::ReviewPanel(AnnotationManager* mgr, QWidget* parent)
    : QWidget(parent)
    , _mgr(mgr)
{
    setMinimumWidth(0);
    setMinimumHeight(160);
    setAttribute(Qt::WA_StyledBackground, false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    _tabWidget = new QTabWidget(this);
    _tabWidget->setStyleSheet(QString(
        "QTabWidget::pane{border:none;background:transparent;}"
        "QTabBar{background:transparent;}"
        "QTabBar::tab{background:transparent;color:%1;padding:10px 11px 11px 11px;font-size:12px;font-weight:550;border-bottom:2px solid transparent;}"
        "QTabBar::tab:selected{color:%2;border-bottom:2px solid rgba(255,140,50,0.90);}"
        "QTabBar::tab:hover{color:%2;}")
        .arg(kSec, kText));

    auto* reviewTab = new QWidget(this);
    auto* metadataTab = new QWidget(this);
    auto* infoTab = new QWidget(this);

    _setupReviewTab(reviewTab);
    _setupMetadataTab(metadataTab);
    _setupInfoTab(infoTab);

    _tabWidget->addTab(reviewTab, QStringLiteral("审阅"));
    _tabWidget->addTab(metadataTab, QStringLiteral("元数据"));
    _tabWidget->addTab(infoTab, QStringLiteral("信息"));

    layout->addWidget(_tabWidget, 1);
    _populateCards({});
}

void ReviewPanel::_setupReviewTab(QWidget* tab)
{
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(12, 12, 12, 10);
    layout->setSpacing(9);

    auto* notesRow = new QHBoxLayout();
    notesRow->setContentsMargins(0, 0, 0, 0);
    notesRow->setSpacing(6);

    auto* notesTitle = new QLabel(QStringLiteral("批注记录"), tab);
    notesTitle->setStyleSheet(QString("color:%1;font-size:12px;font-weight:750;background:transparent;").arg(kText));
    notesRow->addWidget(notesTitle);
    notesRow->addStretch();

    auto* notesPlus = new QPushButton("+", tab);
    notesPlus->setToolTip(QStringLiteral("在当前帧创建批注"));
    notesPlus->setFixedSize(20, 20);
    notesPlus->setCursor(Qt::PointingHandCursor);
    notesPlus->setStyleSheet(QString(
        "QPushButton{color:%1;background:rgba(10,14,18,0.54);border:1px solid %2;border-radius:6px;font-size:14px;font-weight:700;}"
        "QPushButton:hover{color:#ffffff;border-color:rgba(255,255,255,0.16);background:rgba(255,138,61,0.14);}")
        .arg(kSec, kBorder));
    connect(notesPlus, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString text = QInputDialog::getText(
            this,
            QStringLiteral("新建批注"),
            QStringLiteral("内容"),
            QLineEdit::Normal,
            QStringLiteral("新建批注"),
            &ok);
        if (ok && !text.trimmed().isEmpty()) {
            Q_EMIT createNoteRequested(text.trimmed());
        }
    });
    notesRow->addWidget(notesPlus);
    layout->addLayout(notesRow);

    _search = new QLineEdit(tab);
    _search->setPlaceholderText(QStringLiteral("搜索批注内容..."));
    _search->setFixedHeight(30);
    _search->setStyleSheet(QString(
        "QLineEdit{background:rgba(8,12,16,0.48);color:%1;border:1px solid rgba(255,255,255,0.065);border-radius:7px;padding:4px 10px;font-size:12px;}"
        "QLineEdit:focus{border-color:rgba(255,140,50,0.50);background:rgba(10,14,18,0.72);}")
        .arg(kText));
    connect(_search, &QLineEdit::textChanged, this, &ReviewPanel::_filter);
    layout->addWidget(_search);

    auto* filterRow = new QHBoxLayout();
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(8);

    _statusFilter = new QComboBox(tab);
    _statusFilter->addItems({QStringLiteral("全部"), QStringLiteral("待处理"), QStringLiteral("处理中"), QStringLiteral("已解决")});
    _statusFilter->setFixedHeight(30);
    _statusFilter->setStyleSheet(QString(
        "QComboBox{background:rgba(8,12,16,0.48);color:%1;border:1px solid rgba(255,255,255,0.065);border-radius:7px;padding:4px 8px;font-size:11px;}"
        "QComboBox:hover{border-color:rgba(255,140,50,0.32);}"
        "QComboBox QAbstractItemView{background:rgba(17,22,28,0.98);color:%1;border:1px solid rgba(255,255,255,0.08);selection-background-color:rgba(255,140,50,0.18);}")
        .arg(kText));
    connect(_statusFilter, &QComboBox::currentTextChanged, this, &ReviewPanel::_filter);
    filterRow->addWidget(_statusFilter);

    _sortCombo = new QComboBox(tab);
    _sortCombo->addItems({QStringLiteral("按帧排序"), QStringLiteral("按时间排序"), QStringLiteral("按状态排序")});
    _sortCombo->setStyleSheet(_statusFilter->styleSheet());
    connect(_sortCombo, &QComboBox::currentTextChanged, this, &ReviewPanel::_filter);
    filterRow->addWidget(_sortCombo);

    layout->addLayout(filterRow);

    _scrollArea = new QScrollArea(tab);
    _scrollArea->setWidgetResizable(true);
    _scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scrollArea->setStyleSheet("QScrollArea{background:transparent;border:none;}");

    _cardContainer = new QWidget(_scrollArea);
    _cardContainer->setStyleSheet("background:transparent;");
    _cardLayout = new QVBoxLayout(_cardContainer);
    _cardLayout->setContentsMargins(0, 0, 0, 0);
    _cardLayout->setSpacing(10);
    _cardLayout->addStretch();

    _scrollArea->setWidget(_cardContainer);
    layout->addWidget(_scrollArea, 1);

    auto* toolsSection = new QFrame(tab);
    toolsSection->setStyleSheet(QString(
        "QFrame{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 rgba(19,25,32,0.66),stop:1 rgba(10,14,19,0.72));"
        "border:1px solid rgba(255,255,255,0.06);border-radius:10px;}"));
    auto* toolsLayout = new QVBoxLayout(toolsSection);
    toolsLayout->setContentsMargins(10, 10, 10, 10);
    toolsLayout->setSpacing(8);

    auto* toolsTitleRow = new QHBoxLayout();
    toolsTitleRow->setContentsMargins(0, 0, 0, 0);
    toolsTitleRow->setSpacing(6);

    auto* toolsTitle = new QLabel(QStringLiteral("批注工具"), toolsSection);
    toolsTitle->setStyleSheet(QString("color:%1;font-size:11px;font-weight:700;background:transparent;").arg(kText));
    toolsTitleRow->addWidget(toolsTitle);
    toolsTitleRow->addStretch();

    auto* delButton = new QPushButton(QStringLiteral("删除"), toolsSection);
    delButton->setFixedSize(28, 18);
    delButton->setCursor(Qt::PointingHandCursor);
    delButton->setStyleSheet(
        "QPushButton{background:rgba(8,12,16,0.44);color:#ff7070;border:1px solid rgba(255,80,80,0.18);border-radius:5px;font-size:9px;font-weight:700;}"
        "QPushButton:hover{background:rgba(220,60,60,0.14);}");
    connect(delButton, &QPushButton::clicked, this, &ReviewPanel::_deleteSelected);
    toolsTitleRow->addWidget(delButton);
    toolsLayout->addLayout(toolsTitleRow);

    _toolsHost = new QWidget(toolsSection);
    _toolsHost->setStyleSheet("background:transparent;");
    _toolsLayout = new QVBoxLayout(_toolsHost);
    _toolsLayout->setContentsMargins(0, 0, 0, 0);
    _toolsLayout->setSpacing(0);
    toolsLayout->addWidget(_toolsHost);

    layout->addWidget(toolsSection);
}

void ReviewPanel::_populateCards(const QVector<AnnotationItem>& anns)
{
    if (anns.isEmpty()) {
        _selectedCard = nullptr;
        while (QLayoutItem* child = _cardLayout->takeAt(0)) {
            if (child->widget()) {
                child->widget()->deleteLater();
            }
            delete child;
        }
        _cardLayout->addStretch();
        return;
    }

    QVector<AnnotationItem> displayAnns = anns;
    if (displayAnns.isEmpty()) {
        AnnotationItem a;
        a.id = "demo_note_1";
        a.frame = 63;
        a.status = ReviewStatus::Open;
        a.author = "John";
        a.createdTime = "2024-06-01 14:32";
        a.comment = QString::fromUtf8("这里的云层过于自然，需要调整。");
        displayAnns.append(a);

        AnnotationItem b;
        b.id = "demo_note_2";
        b.frame = 252;
        b.status = ReviewStatus::Resolved;
        b.author = "Anna";
        b.createdTime = "2024-06-01 14:35";
        b.comment = QString::fromUtf8("角色背光太强，建议降低亮度。");
        displayAnns.append(b);

        AnnotationItem c;
        c.id = "demo_note_3";
        c.frame = 432;
        c.status = ReviewStatus::InProgress;
        c.author = "Mike";
        c.createdTime = "2024-06-01 14:40";
        c.comment = QString::fromUtf8("山体细节丢失，需要增强对比度。");
        displayAnns.append(c);
    }

    const QString selectedId = _selectedCard ? _selectedCard->property("annId").toString() : QString();
    _selectedCard = nullptr;

    while (QLayoutItem* child = _cardLayout->takeAt(0)) {
        if (child->widget()) {
            child->widget()->deleteLater();
        }
        delete child;
    }

    for (const auto& ann : displayAnns) {
        auto* card = new QFrame(_cardContainer);
        card->setFrameShape(QFrame::StyledPanel);
        card->setCursor(Qt::PointingHandCursor);
        const QColor sc = statusColor(ann.status);
        card->setProperty("statusColor", sc);
        card->setProperty("selected", false);
        card->setProperty("annId", ann.id);
        _applyCardStyle(card, sc, false);

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 11, 12, 11);
        cardLayout->setSpacing(7);

        auto* topRow = new QHBoxLayout();
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(8);

        auto* dot = new QLabel(card);
        dot->setFixedSize(10, 10);
        dot->setStyleSheet(QString("background:%1;border-radius:5px;").arg(sc.name()));
        topRow->addWidget(dot);

        auto* tc = new QLabel(timecodeFromFrame(ann.frame), card);
        tc->setStyleSheet(QString(
            "color:%1;font-size:11px;font-weight:700;background:rgba(13,18,23,0.78);border:1px solid rgba(255,255,255,0.08);border-radius:8px;padding:4px 10px;")
            .arg(kText));
        topRow->addWidget(tc);
        topRow->addStretch();

        auto* editButton = new QPushButton(QStringLiteral("编辑"), card);
        editButton->setFixedHeight(22);
        editButton->setCursor(Qt::PointingHandCursor);
        editButton->setStyleSheet(QString(
            "QPushButton{background:rgba(12,17,22,0.72);color:%1;border:1px solid rgba(255,255,255,0.10);border-radius:6px;padding:0 10px;font-size:10px;font-weight:600;}"
            "QPushButton:hover{border-color:rgba(255,140,50,0.45);background:rgba(255,140,50,0.14);color:#ffffff;}")
            .arg(kText));
        connect(editButton, &QPushButton::clicked, this, [this, annId = ann.id] {
            _requestCommentEdit(annId);
        });
        topRow->addWidget(editButton);
        cardLayout->addLayout(topRow);

        const QString comment = ann.comment.isEmpty() ? QString::fromUtf8("暂无备注") : ann.comment;
        auto* commentLabel = new QLabel(comment, card);
        commentLabel->setWordWrap(true);
        commentLabel->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;border:none;").arg(kText));
        cardLayout->addWidget(commentLabel);

        const QString metaText = QString("%1  ·  %2")
                                     .arg(ann.author.isEmpty() ? QStringLiteral("用户") : ann.author)
                                     .arg(ann.createdTime.isEmpty() ? QStringLiteral("--") : ann.createdTime);
        auto* metaLabel = new QLabel(metaText, card);
        metaLabel->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;border:none;").arg(kSec));
        cardLayout->addWidget(metaLabel);

        card->installEventFilter(this);
        _cardLayout->addWidget(card);

        if (!selectedId.isEmpty() && ann.id == selectedId) {
            _selectedCard = card;
        }
    }

    _cardLayout->addStretch();
    if (_selectedCard) {
        _setSelectedCard(_selectedCard, false);
    }
}

void ReviewPanel::_setupMetadataTab(QWidget* tab)
{
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(12, 12, 12, 12);
    auto* label = new QLabel(
        QStringLiteral("这里显示批注元数据。\n\n选择一条批注后，可查看作者、状态、帧号与时间信息。"),
        tab);
    label->setWordWrap(true);
    label->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;").arg(kSec));
    layout->addWidget(label);
    layout->addStretch();
}

void ReviewPanel::_setupInfoTab(QWidget* tab)
{
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(12, 12, 12, 12);
    auto* label = new QLabel(
        QStringLiteral(
            "CGPlay 2.0\n\n"
            "快捷键\n"
            "Space  播放 / 暂停\n"
            "J / K / L  倒放 / 停止 / 正放\n"
            "Left / Right  上一帧 / 下一帧\n"
            "F  适应窗口\n"
            "F11  全屏\n"
            "Alt + 工具键  批注工具"),
        tab);
    label->setWordWrap(true);
    label->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;").arg(kSec));
    layout->addWidget(label);
    layout->addStretch();
}

void ReviewPanel::_setupSettingsTab(QWidget* tab)
{
    _settingsTabHost = tab;
    auto* layout = new QVBoxLayout(tab);
    _settingsLayout = layout;
    layout->setContentsMargins(12, 12, 12, 12);
    auto* label = new QLabel(
        QStringLiteral(
            "面板设置\n\n"
            "OCIO 相关控制请使用“颜色”菜单。\n"
            "OTIO 导入 / 导出请使用“文件”菜单。"),
        tab);
    label->setWordWrap(true);
    label->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;").arg(kSec));
    _settingsPlaceholder = label;
    layout->addWidget(label);
    layout->addStretch();
}

void ReviewPanel::refresh(const QVector<AnnotationItem>& all, int currentFrame)
{
    _currentFrame = currentFrame;
    QString signature;
    signature.reserve(all.size() * 32);
    for (const auto& ann : all) {
        signature += ann.id;
        signature += QLatin1Char(':');
        signature += QString::number(ann.frame);
        signature += QLatin1Char(':');
        signature += QString::number(static_cast<int>(ann.status));
        signature += QLatin1Char(':');
        signature += ann.comment;
        signature += QLatin1Char('|');
    }

    if (signature == _lastSignature) {
        return;
    }

    _lastSignature = signature;
    _annotations = all;
    _filter();
}

void ReviewPanel::setToolsWidget(QWidget* widget)
{
    if (!widget || !_toolsLayout) {
        return;
    }

    while (QLayoutItem* item = _toolsLayout->takeAt(0)) {
        if (auto* existingWidget = item->widget()) {
            if (existingWidget != widget) {
                existingWidget->hide();
                existingWidget->setParent(nullptr);
            }
        }
        delete item;
    }

    widget->setParent(_toolsHost);
    widget->setVisible(true);
    widget->show();
    _toolsLayout->addWidget(widget);
}

void ReviewPanel::setSettingsWidget(QWidget* widget)
{
    if (!widget || !_settingsLayout || !_settingsTabHost) {
        return;
    }
    if (_settingsWidget == widget) {
        return;
    }

    if (_settingsWidget) {
        _settingsLayout->removeWidget(_settingsWidget);
        _settingsWidget->hide();
        _settingsWidget->deleteLater();
        _settingsWidget = nullptr;
    }
    if (_settingsPlaceholder) {
        _settingsPlaceholder->hide();
    }

    widget->setParent(_settingsTabHost);
    widget->show();
    _settingsLayout->insertWidget(std::max(0, _settingsLayout->count() - 1), widget);
    _settingsWidget = widget;
}

void ReviewPanel::setCurrentTab(int index)
{
    if (_tabWidget && index >= 0 && index < _tabWidget->count()) {
        _tabWidget->setCurrentIndex(index);
    }
}

void ReviewPanel::selectById(const QString& id)
{
    if (_tabWidget) {
        _tabWidget->setCurrentIndex(0);
    }

    if (id.isEmpty()) {
        _clearSelection();
        return;
    }

    for (int i = 0; i < _cardLayout->count(); ++i) {
        auto* card = qobject_cast<QFrame*>(_cardLayout->itemAt(i)->widget());
        if (!card) {
            continue;
        }
        if (card->property("annId").toString() == id) {
            _setSelectedCard(card, false);
            if (_scrollArea) {
                _scrollArea->ensureWidgetVisible(card, 0, 24);
            }
            return;
        }
    }

    _clearSelection();
}

void ReviewPanel::_filter()
{
    const QString text = _search ? _search->text().trimmed().toLower() : QString();
    const int statusIndex = _statusFilter ? _statusFilter->currentIndex() : 0;
    const int sortIndex = _sortCombo ? _sortCombo->currentIndex() : 0;

    QVector<AnnotationItem> filtered;
    for (const auto& ann : _annotations) {
        if (!text.isEmpty() &&
            !ann.comment.toLower().contains(text) &&
            !ann.author.toLower().contains(text)) {
            continue;
        }
        if (statusIndex > 0 && static_cast<int>(ann.status) != statusIndex - 1) {
            continue;
        }
        filtered.append(ann);
    }

    if (sortIndex == 1) {
        std::sort(filtered.begin(), filtered.end(), [](const AnnotationItem& a, const AnnotationItem& b) {
            return a.createdTime > b.createdTime;
        });
    } else if (sortIndex == 2) {
        std::sort(filtered.begin(), filtered.end(), [](const AnnotationItem& a, const AnnotationItem& b) {
            return static_cast<int>(a.status) < static_cast<int>(b.status);
        });
    } else {
        std::sort(filtered.begin(), filtered.end(), [](const AnnotationItem& a, const AnnotationItem& b) {
            return a.frame < b.frame;
        });
    }

    _populateCards(filtered);
}

void ReviewPanel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor panel = surfaceColor(
        "cgplay.panelColor", QColor(QStringLiteral("#171B20")),
        "cgplay.panelOpacity", 96);
    const QColor secondary = surfaceColor(
        "cgplay.backgroundSecondary", panel.darker(112),
        "cgplay.panelOpacity", 96);
    const QColor border = runtimeColor(
        "cgplay.borderColor", QColor(255, 255, 255, 24));
    const QColor accent = runtimeColor(
        "cgplay.accentColor", QColor(0xFF, 0x8A, 0x3D));
    const bool light = qApp &&
        qApp->property("cgplay.themeMode").toString().compare(
            QStringLiteral("light"), Qt::CaseInsensitive) == 0;

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
        QRadialGradient warm(width() * 0.82, height() * 0.08, width() * 0.56);
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
    painter.drawLine(0, 10, 0, height() - 10);
}

void ReviewPanel::_onDoubleClick(QTreeWidgetItem*, int) {}

void ReviewPanel::_onClick(QTreeWidgetItem*, int) {}

void ReviewPanel::_deleteSelected()
{
    if (_selectedCard) {
        const QString id = _selectedCard->property("annId").toString();
        if (!id.isEmpty()) {
            Q_EMIT annotationDeleted(id);
        }
    }
}

void ReviewPanel::_requestCommentEdit(const QString& annId)
{
    if (!_mgr || annId.isEmpty()) {
        return;
    }

    const auto* ann = _mgr->get(annId);
    if (!ann) {
        return;
    }

    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this,
        QStringLiteral("编辑备注"),
        QStringLiteral("备注内容"),
        ann->comment,
        &ok);
    if (!ok) {
        return;
    }

    Q_EMIT annotationCommentEdited(annId, text.trimmed());
}

void ReviewPanel::_setSelectedCard(QFrame* card, bool emitSignal)
{
    if (!card) {
        _clearSelection();
        return;
    }

    if (_selectedCard == card) {
        card->setProperty("selected", true);
        _applyCardStyle(card, card->property("statusColor").value<QColor>(), true);
    } else {
        _clearSelection();
        _selectedCard = card;
        _selectedCard->setProperty("selected", true);
        _applyCardStyle(_selectedCard, _selectedCard->property("statusColor").value<QColor>(), true);
    }

    if (emitSignal) {
        const QString id = card->property("annId").toString();
        if (!id.isEmpty()) {
            Q_EMIT annotationSelected(id);
        }
    }
}

void ReviewPanel::_clearSelection()
{
    if (_selectedCard) {
        _selectedCard->setProperty("selected", false);
        _applyCardStyle(_selectedCard, _selectedCard->property("statusColor").value<QColor>(), false);
        _selectedCard = nullptr;
    }
}

void ReviewPanel::_applyCardStyle(QFrame* card, const QColor& statusColor, bool selected)
{
    if (!card) {
        return;
    }

    const QColor panel = runtimeColor("cgplay.panelColor", QColor(QStringLiteral("#18212B")));
    const QColor accent = runtimeColor("cgplay.accentColor", QColor(QStringLiteral("#FF8A3D")));
    const QColor fill = selected ? QColor::fromRgbF(
        panel.redF() * 0.78 + accent.redF() * 0.22,
        panel.greenF() * 0.78 + accent.greenF() * 0.22,
        panel.blueF() * 0.78 + accent.blueF() * 0.22) : panel;
    const QColor hover = fill.lighter(108);
    const QString borderColor = (selected ? accent : runtimeColor("cgplay.borderColor", QColor(kBorder))).name(QColor::HexArgb);
    const QString fillColor = fill.name(QColor::HexArgb);
    const QString hoverColor = hover.name(QColor::HexArgb);
    card->setStyleSheet(QString(
        "QFrame{background:%1;border:1px solid %2;border-left:3px solid %3;border-radius:10px;}"
        "QFrame:hover{border-color:%4;background:%5;}")
        .arg(fillColor, borderColor, statusColor.name(), accent.name(QColor::HexArgb), hoverColor));
}

bool ReviewPanel::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto* card = qobject_cast<QFrame*>(obj);
        if (card) {
            if (_tabWidget) {
                _tabWidget->setCurrentIndex(0);
            }
            _setSelectedCard(card, true);
            if (_mgr) {
                const QString id = card->property("annId").toString();
                if (const auto* ann = _mgr->get(id)) {
                    Q_EMIT jumpToFrame(ann->frame);
                }
            }
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace cgplay
