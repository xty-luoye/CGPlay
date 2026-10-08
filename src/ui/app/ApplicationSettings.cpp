#include "ApplicationInternal.h"
#include "services/platform/WindowsFileAssociations.h"
#include <QStandardItemModel>
#include <QListWidget>
#include <QTabBar>
#include <QGridLayout>

namespace cgplay {
namespace {

// Search is an index of UI metadata, never a scrape of live editor values.
// In particular, AI credentials, endpoints and runtime status stay out of it.
class SettingsNavigation final : public QObject
{
public:
    SettingsNavigation(QDialog* dialog, QTabWidget* pages, QVBoxLayout* hostLayout)
        : QObject(dialog), _dialog(dialog), _pages(pages)
    {
        _pages->setObjectName(QStringLiteral("SettingsPages"));
        _pages->tabBar()->hide();
        auto* header = new QHBoxLayout();
        auto* heading = new QLabel(QStringLiteral("设置"), dialog);
        heading->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;"));
        header->addWidget(heading);
        header->addStretch();
        _search = new QLineEdit(dialog);
        _search->setObjectName(QStringLiteral("SettingsSearchEdit"));
        _search->setPlaceholderText(QStringLiteral("搜索设置或功能…  Ctrl+F"));
        _search->setAccessibleName(QStringLiteral("搜索设置或功能"));
        _search->setToolTip(QStringLiteral("按名称或命令 ID 查找；Enter 定位，Esc 清空。不会搜索密钥和输入内容。"));
        _search->setClearButtonEnabled(true);
        _search->setMinimumSize(260, 34);
        header->addWidget(_search, 1);
        hostLayout->addLayout(header);

        _results = new QListWidget(dialog);
        _results->setObjectName(QStringLiteral("SettingsSearchResults"));
        _results->setAccessibleName(QStringLiteral("设置搜索结果"));
        _results->setMaximumHeight(170);
        _results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        _results->hide();
        hostLayout->addWidget(_results);
        _empty = new QLabel(QStringLiteral("没有找到匹配项，试试“快捷键”“缓存”或“字幕”。"), dialog);
        _empty->setObjectName(QStringLiteral("SettingsSearchEmptyState"));
        _empty->setWordWrap(true);
        _empty->hide();
        hostLayout->addWidget(_empty);

        auto* body = new QHBoxLayout();
        body->setSpacing(14);
        _categories = new QListWidget(dialog);
        _categories->setObjectName(QStringLiteral("SettingsCategoryNavigation"));
        _categories->setAccessibleName(QStringLiteral("设置分类"));
        _categories->setFixedWidth(140);
        _categories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        for (int i = 0; i < pages->count(); ++i) {
            auto* item = new QListWidgetItem(pages->tabText(i), _categories);
            item->setSizeHint(QSize(130, 38));
            item->setToolTip(pages->tabText(i));
        }
        body->addWidget(_categories);
        auto* content = new QVBoxLayout();
        _title = new QLabel(dialog);
        _title->setObjectName(QStringLiteral("SettingsPageTitle"));
        _title->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
        _description = new QLabel(dialog);
        _description->setObjectName(QStringLiteral("SettingsPageDescription"));
        _description->setWordWrap(true);
        content->addWidget(_title);
        content->addWidget(_description);
        content->addWidget(pages, 1);
        body->addLayout(content, 1);
        hostLayout->addLayout(body, 1);
        connect(_categories, &QListWidget::currentRowChanged, pages, &QTabWidget::setCurrentIndex);
        connect(pages, &QTabWidget::currentChanged, this, [this](int index) { selectCategory(index); });
        selectCategory(pages->currentIndex());

        for (int page = 0; page < pages->count(); ++page) {
            auto* widget = pages->widget(page);
            addEntry(page, pages->tabText(page), widget);
            for (auto* group : widget->findChildren<QGroupBox*>()) addEntry(page, group->title(), group);
            // QFormLayout label roles are static captions, unlike arbitrary
            // QLabel children (which can contain AI errors or user content).
            for (auto* form : widget->findChildren<QFormLayout*>()) {
                for (int row = 0; row < form->rowCount(); ++row) {
                    auto* labelItem = form->itemAt(row, QFormLayout::LabelRole);
                    auto* fieldItem = form->itemAt(row, QFormLayout::FieldRole);
                    auto* label = labelItem ? qobject_cast<QLabel*>(labelItem->widget()) : nullptr;
                    auto* field = fieldItem ? fieldItem->widget() : nullptr;
                    if (!label || !field) continue;
                    if (field->accessibleName().isEmpty()) field->setAccessibleName(label->text());
                    addEntry(page, label->text(), field);
                }
            }
            for (auto* button : widget->findChildren<QAbstractButton*>()) {
                if (button->accessibleName().isEmpty()) button->setAccessibleName(button->text().remove('&'));
                // Enter in search/ordinary edits must never auto-press Save,
                // Reset, Download or an external AI connection action.
                if (auto* push = qobject_cast<QPushButton*>(button)) {
                    push->setAutoDefault(false);
                    push->setDefault(false);
                    push->setMinimumHeight(30);
                }
                addEntry(page, button->text(), button);
            }
            for (auto* field : widget->findChildren<QWidget*>()) {
                const QString metadata = field->property("cgplay.settingsSearchText").toString();
                if (!metadata.isEmpty()) addEntry(page, metadata, field);
            }
        }
        connect(_search, &QLineEdit::textChanged, this, [this] { updateResults(); });
        connect(_results, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { navigate(item); });
        connect(_results, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { navigate(item); });
        qApp->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        auto* widget = qobject_cast<QWidget*>(object);
        if (!widget || widget->window() != _dialog ||
            (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)) return false;
        // Recording Ctrl+F/Esc in a shortcut editor takes precedence over search.
        for (auto* parent = widget; parent; parent = parent->parentWidget())
            if (qobject_cast<QKeySequenceEdit*>(parent)) return false;
        auto* key = static_cast<QKeyEvent*>(event);
        const bool find = key->key() == Qt::Key_F && key->modifiers() == Qt::ControlModifier;
        const bool clear = key->key() == Qt::Key_Escape && !_search->text().isEmpty();
        const bool resultTarget = widget == _search || widget == _results || widget == _results->viewport();
        const bool enter = resultTarget && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter);
        const bool down = widget == _search && key->key() == Qt::Key_Down && _results->count() > 0;
        auto* focusedButton = qobject_cast<QPushButton*>(widget);
        const bool activateButton = focusedButton && focusedButton->hasFocus() && focusedButton->isEnabled() &&
            (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter);
        if (!find && !clear && !enter && !down && !activateButton) return false;
        key->accept();
        if (event->type() == QEvent::ShortcutOverride) return true;
        if (find) { _search->setFocus(Qt::ShortcutFocusReason); _search->selectAll(); }
        else if (clear) { _search->clear(); _search->setFocus(); }
        else if (down) { _results->setCurrentRow(0); _results->setFocus(); }
        else if (enter) navigate(_results->currentItem());
        else if (activateButton && !key->isAutoRepeat()) focusedButton->click();
        return true;
    }

private:
    struct Entry { int page; QString text; QString terms; QPointer<QWidget> target; };
    void addEntry(int page, QString text, QWidget* target)
    {
        text = text.remove('&').simplified();
        if (!target || text.isEmpty()) return;
        for (const auto& entry : _index)
            if (entry.page == page && entry.text == text && entry.target == target) return;
        if (target->objectName().isEmpty())
            target->setObjectName(QStringLiteral("SettingsSearchTarget_%1").arg(_index.size()));
        QString terms = _pages->tabText(page) + QLatin1Char(' ') + text;
        for (auto* parent = target->parentWidget(); parent && parent != _pages; parent = parent->parentWidget())
            if (auto* group = qobject_cast<QGroupBox*>(parent)) terms += QLatin1Char(' ') + group->title();
        _index.push_back({page, text, terms, target});
    }
    void selectCategory(int index)
    {
        if (index < 0 || index >= _pages->count()) return;
        const QSignalBlocker blocker(_categories);
        _categories->setCurrentRow(index);
        _title->setText(_pages->tabText(index));
        const QStringList descriptions{
            QStringLiteral("管理界面面板、色彩入口与软件维护。"),
            QStringLiteral("调整主题、颜色和控件外观；拖动滑块后松开应用。"),
            QStringLiteral("保存工作区布局，导入或导出自定义配置。"),
            QStringLiteral("点击录入快捷键，完成后保存；清空可取消该命令的快捷键。"),
            QStringLiteral("设置工具栏顺序、可见性及组合功能按钮。"),
            QStringLiteral("为鼠标和手柄选择命令，完成后保存绑定。"),
            QStringLiteral("查看全部已注册功能与当前快捷键；此列表不会执行命令。"),
            QStringLiteral("调整音量、音频同步、输出设备与预读。"),
            QStringLiteral("调整硬件解码与缓存；缓存清理不会删除源视频。"),
            QStringLiteral("调整字幕显示、翻译模式和源语言。"),
            QStringLiteral("配置 AI 服务连接；搜索不会读取密钥或编辑框内容。")};
        _description->setText(descriptions.value(index));
    }
    void updateResults()
    {
        _results->clear();
        const auto words = _search->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (int i = 0; !words.isEmpty() && i < _index.size(); ++i) {
            const auto& entry = _index[i];
            bool matches = entry.target;
            for (const auto& word : words) matches = matches && entry.terms.contains(word, Qt::CaseInsensitive);
            if (!matches) continue;
            auto* item = new QListWidgetItem(_pages->tabText(entry.page) + QStringLiteral("  ›  ") + entry.text, _results);
            item->setData(Qt::UserRole, entry.page);
            item->setData(Qt::UserRole + 1, i);
            item->setData(Qt::UserRole + 2, entry.target->objectName());
            item->setToolTip(item->text());
        }
        _results->setVisible(_results->count() > 0);
        _empty->setVisible(!words.isEmpty() && _results->count() == 0);
        if (_results->count()) {
            _results->setFixedHeight(qMin(170, 12 + qMin(5, _results->count()) * qMax(28, _results->sizeHintForRow(0))));
            _results->setCurrentRow(0);
        }
    }
    void navigate(QListWidgetItem* item)
    {
        if (!item) return;
        const int index = item->data(Qt::UserRole + 1).toInt();
        if (index < 0 || index >= _index.size()) return;
        const auto entry = _index[index];
        if (!entry.target) return;
        _pages->setCurrentIndex(entry.page);
        _search->clear();
        // The clear hides the results; settle geometry before scrolling.
        QTimer::singleShot(0, this, [entry] {
            if (!entry.target) return;
            for (auto* parent = entry.target->parentWidget(); parent; parent = parent->parentWidget()) {
                if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
                    scroll->ensureWidgetVisible(entry.target, 12, 24);
                    break;
                }
            }
            if (entry.target->focusPolicy() != Qt::NoFocus) entry.target->setFocus(Qt::OtherFocusReason);
        });
    }
    QDialog* _dialog;
    QTabWidget* _pages;
    QLineEdit* _search = nullptr;
    QListWidget* _results = nullptr;
    QListWidget* _categories = nullptr;
    QLabel* _empty = nullptr;
    QLabel* _title = nullptr;
    QLabel* _description = nullptr;
    QVector<Entry> _index;
};

} // namespace

void MainWindow::_installSettingsWidgets(ReviewPanel* panel)
{
    if (!panel) {
        return;
    }

    if (_p->settingsDialog) {
        auto* existingHost = _p->settingsDialog->findChild<QWidget*>(QStringLiteral("AISettingsWorkspaceWidget"));
        if (existingHost) {
            // This widget has no Q_OBJECT; a typed findChild would use
            // QWidget's metaobject and could cast the tab widget as a wizard.
            auto* namedWizard = existingHost->findChild<QWidget*>(QStringLiteral("AIConnectionWizardWidget"));
            if (auto* existingWizard = dynamic_cast<AIConnectionWizardWidget*>(namedWizard)) {
                existingWizard->refreshFromRuntime();
            }
        }
        return;
    }

    auto* dialog = createThemedDialog(this);
    dialog->setObjectName(QStringLiteral("CGPlaySettingsDialog"));
    dialog->setWindowTitle(QStringLiteral("CGPlay 设置"));
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_StyledBackground, true);
    dialog->setProperty("cgplay.surfaceRole", QStringLiteral("dialog"));
    // Keep the editor compact enough to leave the player visible.  Settings
    // content is scrollable, so a giant dialog only hides the workspace and
    // makes the controls harder to scan.
    dialog->resize(900, 720);
    dialog->setMinimumSize(760, 600);
    auto* dialogLayout = new QVBoxLayout(dialog);
    dialogLayout->setContentsMargins(14, 14, 14, 12);
    dialogLayout->setSpacing(10);

    auto* host = new QWidget(dialog);
    host->setObjectName(QStringLiteral("AISettingsWorkspaceWidget"));
    host->setAttribute(Qt::WA_StyledBackground, true);
    host->setProperty("cgplay.surfaceRole", QStringLiteral("dialog"));
    // Keep this host stylesheet structural only.  Color tokens belong to the
    // application theme stylesheet so a mode change updates the dialog and
    // every child consistently instead of leaving a dark local override.
    host->setStyleSheet(
        "QWidget#AISettingsWorkspaceWidget{background:transparent;}"
        "QTabWidget::pane{border:none;background:transparent;}"
        "QTabBar::tab{padding:8px 6px;font-size:12px;font-weight:600;border-bottom:2px solid transparent;}"
        "QGroupBox{border-radius:6px;margin-top:10px;padding:12px 8px 8px 8px;font-weight:600;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;}"
        "QScrollArea{border:none;background:transparent;}");

    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(8);

    auto* tabs = new QTabWidget(host);
    tabs->setDocumentMode(true);
    tabs->setElideMode(Qt::ElideNone);

    const auto refreshShortcutUi = [this, dialog] {
        auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("SettingsCommandTable"));
        for (const auto& descriptor : _p->commandDescriptors) {
            if (auto* edit = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("SettingsShortcut_") + descriptor.id)) {
                const QSignalBlocker blocker(edit);
                edit->setKeySequence(QKeySequence(descriptor.shortcut));
                if (auto* line = edit->findChild<QLineEdit*>()) line->setPlaceholderText(QStringLiteral("点击设置快捷键"));
            }
            if (table) {
                for (int row = 0; row < table->rowCount(); ++row) {
                    if (table->item(row, 0)->text() != descriptor.id) continue;
                    table->item(row, 2)->setText(descriptor.shortcut);
                    table->item(row, 3)->setText(!descriptor.isEnabled || descriptor.isEnabled()
                        ? QStringLiteral("可用") : QStringLiteral("不可用"));
                    for (int column : {2, 3}) table->item(row, column)->setToolTip(table->item(row, column)->text());
                }
            }
        }
        _refreshCommandPresentation();
    };

    auto makeSettingsPage = [tabs](QWidget** contentOut, QVBoxLayout** layoutOut) {
        auto* scroll = createSettingsScrollArea(tabs);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* content = new QWidget(scroll);
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(4, 6, 4, 10);
        layout->setSpacing(10);
        scroll->setWidget(content);
        *contentOut = content;
        *layoutOut = layout;
        return scroll;
    };

    QWidget* generalContent = nullptr;
    QVBoxLayout* generalLayout = nullptr;
    auto* generalTab = makeSettingsPage(&generalContent, &generalLayout);

    auto* panelGroup = new QGroupBox(QStringLiteral("界面面板"), generalContent);
    auto* panelLayout = new QVBoxLayout(panelGroup);
    auto* playlistVisible = new QCheckBox(QStringLiteral("显示播放列表"), panelGroup);
    auto* reviewVisible = new QCheckBox(QStringLiteral("显示右侧审片面板"), panelGroup);
    auto* annotationVisible = new QCheckBox(QStringLiteral("显示批注工具"), panelGroup);
    auto* aiVisible = new QCheckBox(QStringLiteral("显示 AI 工作台"), panelGroup);
    playlistVisible->setObjectName(QStringLiteral("SettingsPlaylistVisible"));
    reviewVisible->setObjectName(QStringLiteral("SettingsReviewVisible"));
    annotationVisible->setObjectName(QStringLiteral("SettingsAnnotationVisible"));
    aiVisible->setObjectName(QStringLiteral("SettingsAIVisible"));
    playlistVisible->setChecked(_p->leftVisible);
    reviewVisible->setChecked(_p->rightVisible);
    annotationVisible->setChecked(_p->annoToolbar && !_p->annoToolbar->isHidden());
    aiVisible->setChecked(_p->aiDock && !_p->aiDock->isHidden());
    panelLayout->addWidget(playlistVisible);
    panelLayout->addWidget(reviewVisible);
    panelLayout->addWidget(annotationVisible);
    panelLayout->addWidget(aiVisible);
    generalLayout->addWidget(panelGroup);

    connect(playlistVisible, &QCheckBox::toggled, this, [this](bool visible) {
        _p->leftVisible = visible;
        if (visible) {
            _p->activeSidePanel = 1;
        }
        _applyAdaptiveSidePanelLayout(true);
    });
    connect(reviewVisible, &QCheckBox::toggled, this, [this](bool visible) {
        _p->rightVisible = visible;
        if (visible) {
            _p->activeSidePanel = 2;
        }
        _applyAdaptiveSidePanelLayout(true);
    });
    connect(annotationVisible, &QCheckBox::toggled, this, [this](bool visible) {
        _p->annoToolsVisible = visible;
        _onAnnotationModeToggled(visible);
    });
    connect(aiVisible, &QCheckBox::toggled, this, [this](bool visible) {
        if (_p->aiDock) {
            _p->aiDock->setVisible(visible);
            if (visible) _p->aiDock->raise();
        }
    });

    auto* displayGroup = new QGroupBox(QStringLiteral("显示与布局"), generalContent);
    auto* displayLayout = new QVBoxLayout(displayGroup);
    auto* ocioButton = new QPushButton(QStringLiteral("LUT / OCIO 色彩设置"), displayGroup);
    auto* resetLayoutButton = new QPushButton(QStringLiteral("恢复默认面板布局"), displayGroup);
    auto* updateButton = new QPushButton(QStringLiteral("检查版本更新"), displayGroup);
    auto* componentButton = new QPushButton(QStringLiteral("检查缺失组件"), displayGroup);
    auto* resetAllCustomizationButton = new QPushButton(QStringLiteral("恢复所有自定义设置"), displayGroup);
    resetAllCustomizationButton->setObjectName(QStringLiteral("ResetAllCustomizationButton"));
    displayLayout->addWidget(ocioButton);
    displayLayout->addWidget(resetLayoutButton);
    generalLayout->addWidget(displayGroup);
    auto* maintenanceGroup = new QGroupBox(QStringLiteral("软件维护"), generalContent);
    auto* maintenanceLayout = new QVBoxLayout(maintenanceGroup);
    auto* automaticUpdates = new QCheckBox(QStringLiteral("启动时自动检查更新（运行期间每天检查一次）"), maintenanceGroup);
    automaticUpdates->setObjectName(QStringLiteral("AutomaticUpdateCheck"));
    automaticUpdates->setChecked(!_p->userSettings ||
        _p->userSettings->value(QStringLiteral("updates/checkAutomatically"), true).toBool());
    connect(automaticUpdates, &QCheckBox::toggled, this, [this](bool enabled) {
        if (_p->userSettings) {
            _p->userSettings->setValue(QStringLiteral("updates/checkAutomatically"), enabled);
            _p->userSettings->sync();
        }
        if (!enabled && !_p->updateCheckInteractive && _p->updateCheckState)
            _p->updateCheckState->cancelled.store(true);
    });
    maintenanceLayout->addWidget(automaticUpdates);
    maintenanceLayout->addWidget(updateButton);
    maintenanceLayout->addWidget(componentButton);
    generalLayout->addWidget(maintenanceGroup);
    auto* resetGroup = new QGroupBox(QStringLiteral("重置自定义配置"), generalContent);
    auto* resetBox = new QVBoxLayout(resetGroup);
    auto* resetHint = new QLabel(QStringLiteral("恢复外观、工作区和输入绑定。执行前需要确认，不影响源视频。"), resetGroup);
    resetHint->setWordWrap(true);
    resetBox->addWidget(resetHint);
    resetBox->addWidget(resetAllCustomizationButton);
    generalLayout->addWidget(resetGroup);

    auto* associationGroup = new QGroupBox(QStringLiteral("默认播放器与文件关联"), generalContent);
    auto* associationLayout = new QVBoxLayout(associationGroup);
    auto* associationHint = new QLabel(
        QStringLiteral("勾选后点击“应用关联”登记格式；登记不等于当前用户已设为默认。Windows 已有的 UserChoice 需要在系统默认应用中确认。若要清理未锁定格式的旧打开方式，可点击“管理员部署全部默认”；这可能影响本机所有用户，设备策略仍需注销/登录且不绕过 Windows 的 UserChoice 保护。"),
        associationGroup);
    associationHint->setWordWrap(true);
    associationHint->setProperty("cgplay.settingsSearchText", QStringLiteral("默认播放器 文件关联 缩略图 视频格式"));
    associationLayout->addWidget(associationHint);

    auto* associationGrid = new QGridLayout();
    associationGrid->setHorizontalSpacing(18);
    associationGrid->setVerticalSpacing(4);
    auto* associationCount = new QLabel(associationGroup);
    associationCount->setObjectName(QStringLiteral("SettingsFileAssociationCount"));
    associationCount->setMinimumWidth(120);
    auto* selectAllAssociations = new QPushButton(QStringLiteral("全选"), associationGroup);
    selectAllAssociations->setObjectName(QStringLiteral("SettingsFileAssociationSelectAll"));
    auto* clearAssociations = new QPushButton(QStringLiteral("取消全选"), associationGroup);
    clearAssociations->setObjectName(QStringLiteral("SettingsFileAssociationClearAll"));
    auto* applyAssociations = new QPushButton(QStringLiteral("应用关联"), associationGroup);
    applyAssociations->setObjectName(QStringLiteral("SettingsFileAssociationApply"));
    auto* openDefaultApps = new QPushButton(QStringLiteral("打开 CGPlay 默认应用页"), associationGroup);
    openDefaultApps->setObjectName(QStringLiteral("SettingsFileAssociationOpenDefaultApps"));
    auto* deployDeviceDefaults = new QPushButton(QStringLiteral("管理员部署全部默认"), associationGroup);
    deployDeviceDefaults->setObjectName(QStringLiteral("SettingsFileAssociationDeployDeviceDefaults"));
    auto* removeDeviceDefaults = new QPushButton(QStringLiteral("撤销设备级策略"), associationGroup);
    removeDeviceDefaults->setObjectName(QStringLiteral("SettingsFileAssociationRemoveDeviceDefaults"));
    auto* associationButtons = new QHBoxLayout();
    associationButtons->addWidget(associationCount);
    associationButtons->addStretch();
    associationButtons->addWidget(selectAllAssociations);
    associationButtons->addWidget(clearAssociations);
    associationButtons->addWidget(applyAssociations);
    associationButtons->addWidget(openDefaultApps);
    associationLayout->addLayout(associationButtons);
    auto* deviceAssociationButtons = new QHBoxLayout();
    deviceAssociationButtons->addStretch();
    deviceAssociationButtons->addWidget(deployDeviceDefaults);
    deviceAssociationButtons->addWidget(removeDeviceDefaults);
    associationLayout->addLayout(deviceAssociationButtons);

    QVector<QCheckBox*> associationChecks;
    const auto supportedAssociationExtensions = WindowsFileAssociations::supportedVideoExtensions();
    const auto selectedAssociationExtensions = WindowsFileAssociations::selectedVideoExtensions();
    for (int i = 0; i < supportedAssociationExtensions.size(); ++i) {
        const QString extension = supportedAssociationExtensions.at(i).toLower();
        auto* check = new QCheckBox(QStringLiteral(".%1").arg(extension), associationGroup);
        check->setObjectName(QStringLiteral("SettingsFileAssociation_%1").arg(extension));
        check->setProperty("cgplay.settingsSearchText", QStringLiteral("视频 .%1 文件关联").arg(extension));
        check->setChecked(selectedAssociationExtensions.contains(extension));
        associationGrid->addWidget(check, i / 5, i % 5);
        associationChecks.push_back(check);
    }
    associationLayout->addLayout(associationGrid);
    auto* associationStatus = new QLabel(associationGroup);
    associationStatus->setObjectName(QStringLiteral("SettingsFileAssociationStatus"));
    associationStatus->setWordWrap(true);
    associationLayout->addWidget(associationStatus);

    const auto associationStatusText = [supportedAssociationExtensions] {
        const auto effective = WindowsFileAssociations::effectiveDefaultVideoExtensions();
        QStringList pending;
        for (const QString& extension : supportedAssociationExtensions) {
            const QString normalized = extension.toLower();
            if (!effective.contains(normalized)) pending.push_back(QStringLiteral(".%1").arg(normalized));
        }
        QString text = QStringLiteral("当前系统实际使用 CGPlay：%1 / %2 种格式。")
            .arg(effective.size()).arg(supportedAssociationExtensions.size());
        if (!pending.isEmpty()) {
            const int shown = qMin(pending.size(), 8);
            text += QStringLiteral("仍由 Windows 或其他程序控制：%1%2。请在“打开 CGPlay 默认应用页”中确认；程序不会伪造 UserChoice。")
                .arg(pending.mid(0, shown).join(QStringLiteral(", ")))
                .arg(pending.size() > shown ? QStringLiteral(" 等") : QString());
        }
        return text;
    };
    associationStatus->setText(associationStatusText());

    const auto updateAssociationCount = [associationCount, associationChecks] {
        int selected = 0;
        for (auto* check : associationChecks) if (check && check->isChecked()) ++selected;
        associationCount->setText(QStringLiteral("已登记 %1 / %2").arg(selected).arg(associationChecks.size()));
    };
    for (auto* check : associationChecks) {
        connect(check, &QCheckBox::toggled, this, updateAssociationCount);
    }
    connect(selectAllAssociations, &QPushButton::clicked, this, [associationChecks, updateAssociationCount] {
        for (auto* check : associationChecks) if (check) check->setChecked(true);
        updateAssociationCount();
    });
    connect(clearAssociations, &QPushButton::clicked, this, [associationChecks, updateAssociationCount] {
        for (auto* check : associationChecks) if (check) check->setChecked(false);
        updateAssociationCount();
    });
    connect(applyAssociations, &QPushButton::clicked, this,
            [this, associationChecks, associationStatus, associationStatusText, updateAssociationCount] {
        QSet<QString> selected;
        for (auto* check : associationChecks) {
            if (check && check->isChecked()) selected.insert(check->text().mid(1).toLower());
        }
        QString error;
        if (WindowsFileAssociations::applyVideoExtensions(selected, &error)) {
            associationStatus->setText(QStringLiteral("已登记 %1 种视频格式；%2 资源管理器缩略图将在刷新后生效。")
                .arg(selected.size()).arg(associationStatusText()));
            if (statusBar()) statusBar()->showMessage(QStringLiteral("文件关联和视频缩略图设置已应用"), 2800);
        } else {
            associationStatus->setText(error.isEmpty() ? QStringLiteral("文件关联应用失败") : error);
            if (statusBar()) statusBar()->showMessage(associationStatus->text(), 3500);
        }
        updateAssociationCount();
    });
    connect(deployDeviceDefaults, &QPushButton::clicked, this, [this, associationStatus, associationStatusText] {
#ifdef Q_OS_WIN
        const auto choice = QMessageBox::warning(
            this,
            QStringLiteral("部署设备级默认播放器"),
            QStringLiteral("这会请求管理员权限，把 CGPlay 写入本机全部支持的视频格式，并在每次登录时重新应用。可能影响本机其他用户，且会覆盖这些格式的默认播放器。是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
        QString error;
        if (WindowsFileAssociations::requestDeviceDefaultAssociations(&error)) {
            associationStatus->setText(QStringLiteral("设备级策略已部署；%1 请注销并重新登录（或重启）。已有 UserChoice 仍需在 Windows 默认应用中确认。当前安装在用户目录时，其他用户可能无法访问播放器。")
                .arg(associationStatusText()));
            if (statusBar()) statusBar()->showMessage(QStringLiteral("管理员部署完成，注销/登录后生效"), 4500);
        } else {
            associationStatus->setText(error.isEmpty() ? QStringLiteral("设备级默认关联部署失败") : error);
            if (statusBar()) statusBar()->showMessage(associationStatus->text(), 4500);
        }
#else
        Q_UNUSED(associationStatus);
#endif
    });
    connect(removeDeviceDefaults, &QPushButton::clicked, this, [this, associationStatus, associationStatusText] {
#ifdef Q_OS_WIN
        const auto choice = QMessageBox::question(
            this,
            QStringLiteral("撤销设备级默认播放器策略"),
            QStringLiteral("这会请求管理员权限移除 CGPlay 写入的设备级默认关联策略。已有的用户默认选择不会被恢复。是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
        QString error;
        if (WindowsFileAssociations::requestRemoveDeviceDefaultAssociations(&error)) {
            associationStatus->setText(QStringLiteral("CGPlay 设备级策略已撤销；%1 之后可在 Windows 默认应用中重新选择。")
                .arg(associationStatusText()));
            if (statusBar()) statusBar()->showMessage(QStringLiteral("设备级默认关联策略已撤销"), 3500);
        } else {
            associationStatus->setText(error.isEmpty() ? QStringLiteral("设备级默认关联策略撤销失败") : error);
            if (statusBar()) statusBar()->showMessage(associationStatus->text(), 4500);
        }
#else
        Q_UNUSED(associationStatus);
#endif
    });
    connect(openDefaultApps, &QPushButton::clicked, this, [this, associationStatus] {
#ifdef Q_OS_WIN
        // Windows 11 supports a per-user deep link to the registered app page.
        // The Settings page still owns the final UserChoice confirmation and
        // may require one selection per extension. Older Windows builds ignore
        // the query, so fall back to the general Default apps page if launching
        // the deep link fails.
        const QUrl appDefaults(QStringLiteral("ms-settings:defaultapps?registeredAppUser=CGPlay"));
        if (QDesktopServices::openUrl(appDefaults)) {
            associationStatus->setText(QStringLiteral("已打开 CGPlay 默认应用页。当前 Windows 版本没有传统桌面程序的“全部设为默认”按钮，需要按格式选择 CGPlay；程序不会绕过 UserChoice。"));
        } else if (QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps")))) {
            associationStatus->setText(QStringLiteral("已打开 Windows 默认应用页，请选择 CGPlay 并点击“设为默认”。"));
        } else {
            associationStatus->setText(QStringLiteral("无法打开 Windows 默认应用设置，请手动打开“设置 → 应用 → 默认应用”。"));
        }
#else
        if (statusBar()) statusBar()->showMessage(QStringLiteral("Windows 默认应用设置仅在 Windows 上可用"), 2500);
#endif
    });
#ifndef Q_OS_WIN
    associationStatus->setText(QStringLiteral("文件关联与资源管理器缩略图仅支持 Windows。"));
    selectAllAssociations->setEnabled(false);
    clearAssociations->setEnabled(false);
    applyAssociations->setEnabled(false);
    openDefaultApps->setEnabled(false);
    deployDeviceDefaults->setEnabled(false);
    removeDeviceDefaults->setEnabled(false);
#else
    updateAssociationCount();
#endif
    generalLayout->addWidget(associationGroup);
    generalLayout->addStretch();
    connect(ocioButton, &QPushButton::clicked, this, [this] {
        if (_p->ocioManager) _p->ocioManager->showSettings(this);
    });
    connect(resetLayoutButton, &QPushButton::clicked, this, [this, playlistVisible, reviewVisible] {
        _p->leftVisible = true;
        _p->rightVisible = true;
        _p->activeSidePanel = 2;
        if (_p->compareBar) _p->compareBar->hide();
        _applyAdaptiveSidePanelLayout(true);
        playlistVisible->setChecked(_p->leftVisible);
        reviewVisible->setChecked(_p->rightVisible);
    });
    connect(updateButton, &QPushButton::clicked, this, [this] { _checkForUpdates(true); });
    connect(componentButton, &QPushButton::clicked, this, [this] { _checkVersionAndComponents(true); });
    tabs->addTab(generalTab, QStringLiteral("常规"));

    QWidget* appearanceContent = nullptr;
    QVBoxLayout* appearanceLayout = nullptr;
    auto* appearanceTab = makeSettingsPage(&appearanceContent, &appearanceLayout);
    auto* themeGroup = new QGroupBox(QStringLiteral("播放器外观"), appearanceContent);
    auto* themeForm = new QFormLayout(themeGroup);
    auto* themeMode = new QComboBox(themeGroup);
    themeMode->addItem(QStringLiteral("深色"), QStringLiteral("dark"));
    themeMode->addItem(QStringLiteral("浅色"), QStringLiteral("light"));
    themeMode->addItem(QStringLiteral("半透明玻璃"), QStringLiteral("glass"));
    themeMode->addItem(QStringLiteral("高对比度"), QStringLiteral("highContrast"));
    themeMode->addItem(QStringLiteral("自定义"), QStringLiteral("custom"));
    const QString savedThemeMode = _p->userSettings ? _p->userSettings->value(QStringLiteral("appearance/mode"), QStringLiteral("dark")).toString() : QStringLiteral("dark");
    const int themeModeIndex = themeMode->findData(savedThemeMode); if (themeModeIndex >= 0) themeMode->setCurrentIndex(themeModeIndex);
    themeForm->addRow(QStringLiteral("主题模式"), themeMode);
    const auto activateCustomTheme = [this, themeMode] {
        if (!_p->userSettings || themeMode->currentData().toString() == QStringLiteral("custom")) return;
        const QSignalBlocker blocker(themeMode);
        const int customIndex = themeMode->findData(QStringLiteral("custom"));
        if (customIndex >= 0) themeMode->setCurrentIndex(customIndex);
        _p->userSettings->setValue(QStringLiteral("appearance/mode"), QStringLiteral("custom"));
    };
    const auto refreshAppearanceImmediately = [] {
        // Discrete operations (color pickers, theme/background selection and
        // explicit Apply/Reset) need an immediate visible result. Continuous
        // sliders use the separate pending-refresh path below.
        const bool deferred = qApp->property("cgplay.deferAppearanceRefresh").toBool();
        qApp->setProperty("cgplay.deferAppearanceRefresh", false);
        if (auto* app = qobject_cast<Application*>(qApp)) app->refreshAppearanceSettings();
        qApp->setProperty("cgplay.deferAppearanceRefresh", deferred);
    };
    const auto colorButtonText = [](QColor color) {
        if (!color.isValid()) {
            return QStringLiteral("#FFFFFF");
        }
        // Color swatches must remain readable even when the selected color is
        // a light custom value.  The previous fixed white text disappeared on
        // light swatches and made the editor look broken.
        return color.lightnessF() > 0.62 ? QStringLiteral("#111418") : QStringLiteral("#FFFFFF");
    };
    auto makeColorButton = [this, themeGroup, themeMode, activateCustomTheme, refreshAppearanceImmediately, colorButtonText](const QString& key, const QString& label, const QColor& fallback) {
        auto* button = new QPushButton(label, themeGroup);
        // A color picker is a compact field, not a stretchable content panel.
        // Fixed dimensions also keep QFormLayout from turning swatches into
        // the oversized blocks seen in the appearance editor.
        button->setMinimumSize(190, 30);
        button->setMaximumWidth(240);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        const auto runtimeFallback = [key, fallback] {
            const auto propertyForKey = [&key]() -> const char* {
                if (key == QStringLiteral("appearance/backgroundColor")) return "cgplay.backgroundColor";
                if (key == QStringLiteral("appearance/backgroundSecondary")) return "cgplay.backgroundSecondary";
                if (key == QStringLiteral("appearance/panelColor") || key.contains(QStringLiteral("/buttonColor/"))) return "cgplay.panelColor";
                if (key == QStringLiteral("appearance/toolbarColor")) return "cgplay.toolbarColor";
                if (key == QStringLiteral("appearance/timelineColor")) return "cgplay.timelineColor";
                if (key == QStringLiteral("appearance/textColor") || key.contains(QStringLiteral("/buttonText/")) || key.contains(QStringLiteral("/buttonIcon/"))) return "cgplay.textColor";
                if (key == QStringLiteral("appearance/borderColor") || key.contains(QStringLiteral("/buttonBorder/"))) return "cgplay.borderColor";
                if (key.contains(QStringLiteral("/buttonHover/")) || key.contains(QStringLiteral("/buttonPressed/"))) return "cgplay.accentColor";
                if (key == QStringLiteral("appearance/accentColor")) return "cgplay.accentColor";
                return nullptr;
            }();
            if (propertyForKey && qApp) {
                const QColor value(qApp->property(propertyForKey).toString());
                if (value.isValid()) return value;
            }
            return fallback;
        };
        const QColor effectiveFallback = runtimeFallback();
        QColor current = _p->userSettings ? QColor(_p->userSettings->value(key, effectiveFallback.name(QColor::HexArgb)).toString()) : effectiveFallback;
        if (!current.isValid()) current = effectiveFallback;
        button->setProperty("cgplay.appearanceKey", key);
        button->setProperty("cgplay.appearanceFallbackColor", effectiveFallback.name(QColor::HexArgb));
        button->setProperty("selectedColor", current.name(QColor::HexArgb));
        button->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;min-height:30px;}")
            .arg(current.name(QColor::HexArgb), colorButtonText(current)));
        connect(button, &QPushButton::clicked, this, [this, button, key, effectiveFallback, activateCustomTheme, refreshAppearanceImmediately, colorButtonText] {
            QColor initial = _p->userSettings ? QColor(_p->userSettings->value(key, effectiveFallback.name(QColor::HexArgb)).toString()) : effectiveFallback;
            if (!initial.isValid()) initial = effectiveFallback;
            const QColor color = QColorDialog::getColor(initial, this, QStringLiteral("选择颜色"));
            if (!color.isValid()) return;
            activateCustomTheme();
            if (_p->userSettings) _p->userSettings->setValue(key, color.name(QColor::HexArgb));
            button->setProperty("selectedColor", color.name(QColor::HexArgb));
            button->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;min-height:30px;}")
                .arg(color.name(QColor::HexArgb), colorButtonText(color)));
            refreshAppearanceImmediately();
        });
        return button;
    };
    themeForm->addRow(QStringLiteral("主背景颜色"), makeColorButton(QStringLiteral("appearance/backgroundColor"), QStringLiteral("选择背景颜色"), QColor(QStringLiteral("#10161D"))));
    themeForm->addRow(QStringLiteral("背景渐变结束色"), makeColorButton(QStringLiteral("appearance/backgroundSecondary"), QStringLiteral("选择背景结束色"), QColor(QStringLiteral("#18212B"))));
    auto* panelColorButton = makeColorButton(QStringLiteral("appearance/panelColor"), QStringLiteral("选择面板颜色"), QColor(QStringLiteral("#18212B")));
    themeForm->addRow(QStringLiteral("面板颜色"), panelColorButton);
    themeForm->addRow(QStringLiteral("工具栏颜色"), makeColorButton(QStringLiteral("appearance/toolbarColor"), QStringLiteral("选择工具栏颜色"), QColor(QStringLiteral("#18212B"))));
    themeForm->addRow(QStringLiteral("时间轴颜色"), makeColorButton(QStringLiteral("appearance/timelineColor"), QStringLiteral("选择时间轴颜色"), QColor(QStringLiteral("#0D1218"))));
    auto* textColorButton = makeColorButton(QStringLiteral("appearance/textColor"), QStringLiteral("选择字体颜色"), QColor(QStringLiteral("#D8DEE7")));
    themeForm->addRow(QStringLiteral("字体颜色"), textColorButton);
    themeForm->addRow(QStringLiteral("边框颜色"), makeColorButton(QStringLiteral("appearance/borderColor"), QStringLiteral("选择边框颜色"), QColor(QStringLiteral("#303B47"))));
    themeForm->addRow(QStringLiteral("强调色"), makeColorButton(QStringLiteral("appearance/accentColor"), QStringLiteral("选择强调色"), QColor(QStringLiteral("#FF8A3D"))));
    auto* contrastWarning = new QLabel(themeGroup);
    contrastWarning->setWordWrap(true);
    const auto updateContrastWarning = [this, contrastWarning] {
        const QColor text = _p->userSettings
            ? QColor(_p->userSettings->value(QStringLiteral("appearance/textColor"), QStringLiteral("#D8DEE7")).toString())
            : QColor(QStringLiteral("#D8DEE7"));
        const QColor panel = _p->userSettings
            ? QColor(_p->userSettings->value(QStringLiteral("appearance/panelColor"), QStringLiteral("#18212B")).toString())
            : QColor(QStringLiteral("#18212B"));
        const auto luminance = [](const QColor& color) {
            const auto channel = [](double value) {
                value /= 255.0;
                return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            };
            return channel(color.red()) * 0.2126 + channel(color.green()) * 0.7152 + channel(color.blue()) * 0.0722;
        };
        const double a = luminance(text);
        const double b = luminance(panel);
        const double ratio = (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
        contrastWarning->setVisible(ratio < 4.5);
        contrastWarning->setText(QStringLiteral("文字与面板对比度仅 %1:1，建议调整到 4.5:1 以上。").arg(ratio, 0, 'f', 1));
    };
    updateContrastWarning();
    connect(panelColorButton, &QPushButton::clicked, this, [updateContrastWarning] { QTimer::singleShot(0, updateContrastWarning); });
    connect(textColorButton, &QPushButton::clicked, this, [updateContrastWarning] { QTimer::singleShot(0, updateContrastWarning); });
    themeForm->addRow(QString(), contrastWarning);
    auto* backgroundType = new QComboBox(themeGroup);
    backgroundType->addItem(QStringLiteral("纯色"), QStringLiteral("solid"));
    backgroundType->addItem(QStringLiteral("渐变色"), QStringLiteral("gradient"));
    backgroundType->addItem(QStringLiteral("本地图片"), QStringLiteral("image"));
    backgroundType->addItem(QStringLiteral("纹理"), QStringLiteral("texture"));
    const QString savedBackgroundType = _p->userSettings
        ? _p->userSettings->value(QStringLiteral("appearance/backgroundType"), QStringLiteral("solid")).toString()
        : QStringLiteral("solid");
    const int backgroundTypeIndex = backgroundType->findData(savedBackgroundType);
    if (backgroundTypeIndex >= 0) backgroundType->setCurrentIndex(backgroundTypeIndex);
    themeForm->addRow(QStringLiteral("背景类型"), backgroundType);
    auto* backgroundImage = new QLineEdit(themeGroup);
    backgroundImage->setPlaceholderText(QStringLiteral("选择本地图片或纹理文件"));
    backgroundImage->setText(_p->userSettings ? _p->userSettings->value(QStringLiteral("appearance/backgroundImage")).toString() : QString());
    auto* browseBackground = new QPushButton(QStringLiteral("浏览"), themeGroup);
    browseBackground->setFixedWidth(64);
    auto* backgroundPathHost = new QWidget(themeGroup);
    auto* backgroundPathLayout = new QHBoxLayout(backgroundPathHost);
    backgroundPathLayout->setContentsMargins(0, 0, 0, 0);
    backgroundPathLayout->addWidget(backgroundImage, 1);
    backgroundPathLayout->addWidget(browseBackground);
    themeForm->addRow(QStringLiteral("背景文件"), backgroundPathHost);
    auto* backgroundFillMode = new QComboBox(themeGroup);
    backgroundFillMode->addItem(QStringLiteral("填充"), QStringLiteral("cover"));
    backgroundFillMode->addItem(QStringLiteral("完整显示"), QStringLiteral("contain"));
    backgroundFillMode->addItem(QStringLiteral("平铺"), QStringLiteral("tile"));
    const QString savedFillMode = _p->userSettings
        ? _p->userSettings->value(QStringLiteral("appearance/fillMode"), QStringLiteral("cover")).toString()
        : QStringLiteral("cover");
    const int fillModeIndex = backgroundFillMode->findData(savedFillMode);
    if (fillModeIndex >= 0) backgroundFillMode->setCurrentIndex(fillModeIndex);
    themeForm->addRow(QStringLiteral("填充模式"), backgroundFillMode);
    appearanceLayout->addWidget(themeGroup);

    auto* themePresetGroup = new QGroupBox(QStringLiteral("主题预设"), appearanceContent);
    auto* themePresetLayout = new QHBoxLayout(themePresetGroup);
    auto* savedThemes = new QComboBox(themePresetGroup);
    auto* saveThemePreset = new QPushButton(QStringLiteral("保存"), themePresetGroup);
    auto* loadThemePreset = new QPushButton(QStringLiteral("加载"), themePresetGroup);
    auto* deleteThemePreset = new QPushButton(QStringLiteral("删除"), themePresetGroup);
    const auto reloadThemeNames = [savedThemes](const QString& selected = QString()) {
        const QSignalBlocker blocker(savedThemes);
        savedThemes->clear();
        savedThemes->addItems(ThemeService::names());
        const int index = savedThemes->findText(selected);
        if (index >= 0) savedThemes->setCurrentIndex(index);
    };
    reloadThemeNames();
    themePresetLayout->addWidget(savedThemes, 1);
    themePresetLayout->addWidget(saveThemePreset);
    themePresetLayout->addWidget(loadThemePreset);
    themePresetLayout->addWidget(deleteThemePreset);
    appearanceLayout->addWidget(themePresetGroup);

    const auto currentThemeDefinition = [this](const QString& name) {
        ThemeDefinition theme;
        theme.name = name;
        if (!_p->userSettings) return theme;
        theme.mode = themeModeFromName(_p->userSettings->value(QStringLiteral("appearance/mode"), QStringLiteral("dark")).toString());
        theme.backgroundType = backgroundTypeFromName(_p->userSettings->value(QStringLiteral("appearance/backgroundType"), QStringLiteral("solid")).toString());
        const auto color = [this](const QString& key, const QColor& fallback) {
            const QColor value(_p->userSettings->value(QStringLiteral("appearance/") + key, fallback.name(QColor::HexArgb)).toString());
            return value.isValid() ? value : fallback;
        };
        theme.background = color(QStringLiteral("backgroundColor"), theme.background);
        theme.backgroundSecondary = color(QStringLiteral("backgroundSecondary"), theme.backgroundSecondary);
        theme.panel = color(QStringLiteral("panelColor"), theme.panel);
        theme.toolbar = color(QStringLiteral("toolbarColor"), theme.toolbar);
        theme.timeline = color(QStringLiteral("timelineColor"), theme.timeline);
        theme.text = color(QStringLiteral("textColor"), theme.text);
        theme.border = color(QStringLiteral("borderColor"), theme.border);
        theme.accent = color(QStringLiteral("accentColor"), theme.accent);
        theme.backgroundImage = _p->userSettings->value(QStringLiteral("appearance/backgroundImage")).toString();
        theme.texturePath = _p->userSettings->value(QStringLiteral("appearance/texturePath")).toString();
        theme.fillMode = _p->userSettings->value(QStringLiteral("appearance/fillMode"), QStringLiteral("cover")).toString();
        theme.backgroundOpacity = _p->userSettings->value(QStringLiteral("appearance/backgroundOpacity"), 100).toInt();
        theme.panelOpacity = _p->userSettings->value(QStringLiteral("appearance/panelOpacity"), 96).toInt();
        theme.toolbarOpacity = _p->userSettings->value(QStringLiteral("appearance/toolbarOpacity"), 92).toInt();
        theme.timelineOpacity = _p->userSettings->value(QStringLiteral("appearance/timelineOpacity"), 96).toInt();
        theme.subtitleOpacity = _p->userSettings->value(QStringLiteral("appearance/subtitleOpacity"), 86).toInt();
        theme.viewerOpacity = _p->userSettings->value(QStringLiteral("appearance/viewerOpacity"), 100).toInt();
        theme.shadowStrength = _p->userSettings->value(QStringLiteral("appearance/shadowStrength"), 25).toInt();
        theme.blurRadius = _p->userSettings->value(QStringLiteral("appearance/blurRadius"), 0).toInt();
        theme.vignette = _p->userSettings->value(QStringLiteral("appearance/vignette"), 0).toInt();
        theme.brightness = _p->userSettings->value(QStringLiteral("appearance/brightness"), 100).toInt();
        theme.saturation = _p->userSettings->value(QStringLiteral("appearance/saturation"), 100).toInt();
        theme.dynamicBackground = _p->userSettings->value(QStringLiteral("appearance/dynamicBackground"), false).toBool();
        theme.normalize();
        return theme;
    };
    connect(saveThemePreset, &QPushButton::clicked, this, [this, currentThemeDefinition, reloadThemeNames] {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("保存主题"), QStringLiteral("主题名称"),
            QLineEdit::Normal, QStringLiteral("自定义主题"), &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        QString error;
        if (!ThemeService::save(currentThemeDefinition(name), &error)) {
            if (statusBar()) statusBar()->showMessage(QStringLiteral("主题保存失败：%1").arg(error), 3500);
            return;
        }
        reloadThemeNames(ThemeService::sanitizeName(name));
        if (statusBar()) statusBar()->showMessage(QStringLiteral("主题已保存：%1").arg(name), 2500);
    });
    connect(loadThemePreset, &QPushButton::clicked, this, [this, savedThemes, appearanceContent, themeMode, backgroundType, backgroundImage, backgroundFillMode, colorButtonText] {
        const QString name = savedThemes->currentText();
        if (name.isEmpty() || !_p->userSettings) return;
        bool recovered = false;
        QString error;
        const ThemeDefinition theme = ThemeService::load(name, &recovered, &error);
        _p->userSettings->setValue(QStringLiteral("appearance/mode"), themeModeName(theme.mode));
        _p->userSettings->setValue(QStringLiteral("appearance/backgroundType"), backgroundTypeName(theme.backgroundType));
        const QList<QPair<QString, QColor>> colors = {
            {QStringLiteral("backgroundColor"), theme.background}, {QStringLiteral("backgroundSecondary"), theme.backgroundSecondary},
            {QStringLiteral("panelColor"), theme.panel}, {QStringLiteral("toolbarColor"), theme.toolbar},
            {QStringLiteral("timelineColor"), theme.timeline}, {QStringLiteral("textColor"), theme.text},
            {QStringLiteral("borderColor"), theme.border}, {QStringLiteral("accentColor"), theme.accent}};
        for (const auto& entry : colors) _p->userSettings->setValue(QStringLiteral("appearance/") + entry.first, entry.second.name(QColor::HexArgb));
        _p->userSettings->setValue(QStringLiteral("appearance/backgroundImage"), theme.backgroundImage);
        _p->userSettings->setValue(QStringLiteral("appearance/texturePath"), theme.texturePath);
        _p->userSettings->setValue(QStringLiteral("appearance/fillMode"), theme.fillMode);
        const QList<QPair<QString, int>> values = {
            {QStringLiteral("backgroundOpacity"), theme.backgroundOpacity}, {QStringLiteral("panelOpacity"), theme.panelOpacity},
            {QStringLiteral("toolbarOpacity"), theme.toolbarOpacity}, {QStringLiteral("timelineOpacity"), theme.timelineOpacity},
            {QStringLiteral("subtitleOpacity"), theme.subtitleOpacity}, {QStringLiteral("viewerOpacity"), theme.viewerOpacity},
            {QStringLiteral("shadowStrength"), theme.shadowStrength}, {QStringLiteral("blurRadius"), theme.blurRadius},
            {QStringLiteral("vignette"), theme.vignette}, {QStringLiteral("brightness"), theme.brightness},
            {QStringLiteral("saturation"), theme.saturation}};
        for (const auto& entry : values) _p->userSettings->setValue(QStringLiteral("appearance/") + entry.first, entry.second);
        _p->userSettings->setValue(QStringLiteral("appearance/dynamicBackground"), theme.dynamicBackground);
        _p->userSettings->sync();

        { const QSignalBlocker blocker(themeMode); themeMode->setCurrentIndex(themeMode->findData(themeModeName(theme.mode))); }
        { const QSignalBlocker blocker(backgroundType); backgroundType->setCurrentIndex(backgroundType->findData(backgroundTypeName(theme.backgroundType))); }
        { const QSignalBlocker blocker(backgroundImage); backgroundImage->setText(theme.backgroundImage); }
        { const QSignalBlocker blocker(backgroundFillMode); backgroundFillMode->setCurrentIndex(backgroundFillMode->findData(theme.fillMode)); }
        for (QPushButton* button : appearanceContent->findChildren<QPushButton*>()) {
            const QString key = button->property("cgplay.appearanceKey").toString();
            if (key.isEmpty()) continue;
            const QColor selected(_p->userSettings->value(key).toString());
            if (!selected.isValid()) continue;
            button->setProperty("selectedColor", selected.name(QColor::HexArgb));
            button->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;min-height:30px;}")
                .arg(selected.name(QColor::HexArgb), colorButtonText(selected)));
        }
        for (LiveSlider* slider : appearanceContent->findChildren<LiveSlider*>()) {
            const QString key = slider->property("cgplay.appearanceKey").toString();
            if (key.isEmpty() || !_p->userSettings->contains(key)) continue;
            const QSignalBlocker blocker(slider);
            slider->setSmoothValue(_p->userSettings->value(key).toInt());
        }
        for (QSpinBox* spin : appearanceContent->findChildren<QSpinBox*>()) {
            const QString key = spin->property("cgplay.appearanceKey").toString();
            if (key.isEmpty() || !_p->userSettings->contains(key)) continue;
            const QSignalBlocker blocker(spin);
            spin->setValue(_p->userSettings->value(key).toInt());
        }
        const bool deferred = qApp->property("cgplay.deferAppearanceRefresh").toBool();
        qApp->setProperty("cgplay.deferAppearanceRefresh", false);
        if (auto* app = qobject_cast<Application*>(qApp)) app->refreshAppearanceSettings();
        qApp->setProperty("cgplay.deferAppearanceRefresh", deferred);
        if (statusBar()) statusBar()->showMessage(recovered ? QStringLiteral("主题已从最近有效版本恢复") : QStringLiteral("主题已加载：%1").arg(name), 3000);
    });
    connect(deleteThemePreset, &QPushButton::clicked, this, [this, savedThemes, reloadThemeNames] {
        const QString name = savedThemes->currentText();
        if (name.isEmpty()) return;
        QString error;
        if (!ThemeService::remove(name, &error)) {
            if (statusBar()) statusBar()->showMessage(QStringLiteral("主题删除失败：%1").arg(error), 3000);
            return;
        }
        reloadThemeNames();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("主题已删除：%1").arg(name), 2500);
    });
    connect(themeMode, &QComboBox::currentIndexChanged, this, [this, appearanceContent, themeMode, refreshAppearanceImmediately](int) {
        if (_p->userSettings) _p->userSettings->setValue(QStringLiteral("appearance/mode"), themeMode->currentData().toString());
        refreshAppearanceImmediately();
        const QHash<QString, QByteArray> properties = {
            {QStringLiteral("appearance/buttonOpacity"), QByteArrayLiteral("cgplay.buttonOpacity")},
            {QStringLiteral("appearance/backgroundOpacity"), QByteArrayLiteral("cgplay.backgroundOpacity")},
            {QStringLiteral("appearance/panelOpacity"), QByteArrayLiteral("cgplay.panelOpacity")},
            {QStringLiteral("appearance/toolbarOpacity"), QByteArrayLiteral("cgplay.toolbarOpacity")},
            {QStringLiteral("appearance/timelineOpacity"), QByteArrayLiteral("cgplay.timelineOpacity")},
            {QStringLiteral("appearance/subtitleOpacity"), QByteArrayLiteral("cgplay.subtitleOpacity")},
            {QStringLiteral("appearance/viewerOpacity"), QByteArrayLiteral("cgplay.viewerOpacity")},
            {QStringLiteral("appearance/brightness"), QByteArrayLiteral("cgplay.backgroundBrightness")},
            {QStringLiteral("appearance/saturation"), QByteArrayLiteral("cgplay.backgroundSaturation")},
            {QStringLiteral("appearance/blurRadius"), QByteArrayLiteral("cgplay.backgroundBlurRadius")},
            {QStringLiteral("appearance/vignette"), QByteArrayLiteral("cgplay.backgroundVignette")},
            {QStringLiteral("appearance/shadowStrength"), QByteArrayLiteral("cgplay.shadowStrength")}};
        const auto effectiveValue = [&properties](QObject* editor) {
            const QString key = editor->property("cgplay.appearanceKey").toString();
            if (key == QStringLiteral("appearance/windowOpacity")) return 100;
            const QByteArray propertyName = properties.value(key);
            return propertyName.isEmpty() ? editor->property("cgplay.appearanceFallback").toInt()
                                          : qApp->property(propertyName.constData()).toInt();
        };
        for (auto* slider : appearanceContent->findChildren<LiveSlider*>()) {
            if (slider->property("cgplay.appearanceKey").toString().isEmpty()) continue;
            const QSignalBlocker blocker(slider);
            slider->setSmoothValue(effectiveValue(slider));
        }
        for (auto* editor : appearanceContent->findChildren<QSpinBox*>()) {
            if (editor->property("cgplay.appearanceKey").toString().isEmpty()) continue;
            const QSignalBlocker blocker(editor);
            editor->setValue(effectiveValue(editor));
        }
    });
    const auto saveBackgroundSetting = [this, activateCustomTheme, refreshAppearanceImmediately](const QString& key, const QVariant& value) {
        activateCustomTheme();
        if (_p->userSettings) _p->userSettings->setValue(key, value);
        refreshAppearanceImmediately();
    };
    connect(backgroundType, &QComboBox::currentIndexChanged, this, [backgroundType, saveBackgroundSetting](int) {
        saveBackgroundSetting(QStringLiteral("appearance/backgroundType"), backgroundType->currentData().toString());
    });
    connect(backgroundFillMode, &QComboBox::currentIndexChanged, this, [backgroundFillMode, saveBackgroundSetting](int) {
        saveBackgroundSetting(QStringLiteral("appearance/fillMode"), backgroundFillMode->currentData().toString());
    });
    connect(backgroundImage, &QLineEdit::editingFinished, this, [backgroundImage, backgroundType, saveBackgroundSetting] {
        const QString path = backgroundImage->text().trimmed();
        saveBackgroundSetting(QStringLiteral("appearance/backgroundImage"), path);
        // A path alone is not enough to select the image renderer.  Make the
        // text-field workflow behave like the Browse button and switch to the
        // image mode when a real image path is entered.
        if (!path.isEmpty() && QFileInfo::exists(path) && backgroundType->currentData().toString() == QStringLiteral("solid")) {
            const QSignalBlocker blocker(backgroundType);
            const int imageIndex = backgroundType->findData(QStringLiteral("image"));
            if (imageIndex >= 0) backgroundType->setCurrentIndex(imageIndex);
            saveBackgroundSetting(QStringLiteral("appearance/backgroundType"), QStringLiteral("image"));
        }
    });
    connect(browseBackground, &QPushButton::clicked, this, [this, backgroundImage, backgroundType, saveBackgroundSetting] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择背景文件"), backgroundImage->text(), QStringLiteral("图片/纹理 (*.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff);;所有文件 (*.*)"));
        if (path.isEmpty()) return;
        backgroundImage->setText(path);
        saveBackgroundSetting(QStringLiteral("appearance/backgroundImage"), path);
        const QSignalBlocker blocker(backgroundType);
        const int imageIndex = backgroundType->findData(QStringLiteral("image"));
        if (imageIndex >= 0) backgroundType->setCurrentIndex(imageIndex);
        saveBackgroundSetting(QStringLiteral("appearance/backgroundType"), QStringLiteral("image"));
    });
    auto* resetTheme = new QPushButton(QStringLiteral("恢复主题默认"), themeGroup);
    themeForm->addRow(QString(), resetTheme);
    connect(resetTheme, &QPushButton::clicked, this, [this, appearanceContent, themeMode, backgroundType, backgroundImage, backgroundFillMode, refreshAppearanceImmediately, colorButtonText] {
        if (_p->userSettings) {
             for (const QString& key : {QStringLiteral("mode"), QStringLiteral("backgroundColor"), QStringLiteral("backgroundSecondary"), QStringLiteral("panelColor"), QStringLiteral("toolbarColor"), QStringLiteral("timelineColor"), QStringLiteral("textColor"), QStringLiteral("borderColor"), QStringLiteral("accentColor"), QStringLiteral("backgroundType"), QStringLiteral("backgroundImage"), QStringLiteral("texturePath"), QStringLiteral("fillMode"), QStringLiteral("backgroundOpacity"), QStringLiteral("vignette"), QStringLiteral("blurRadius"), QStringLiteral("brightness"), QStringLiteral("saturation"), QStringLiteral("shadowStrength"), QStringLiteral("dynamicBackground"), QStringLiteral("windowOpacity"), QStringLiteral("buttonOpacity"), QStringLiteral("panelOpacity"), QStringLiteral("toolbarOpacity"), QStringLiteral("timelineOpacity"), QStringLiteral("subtitleOpacity"), QStringLiteral("viewerOpacity")}) _p->userSettings->remove(QStringLiteral("appearance/") + key);
         }
         {
             const QSignalBlocker blocker(themeMode);
             const int darkIndex = themeMode->findData(QStringLiteral("dark"));
             if (darkIndex >= 0) themeMode->setCurrentIndex(darkIndex);
         }
        // Keep the editor controls in sync with the reset runtime state.  A
        // reset used to clear QSettings but leave the old image/gradient
        // selection visible, so the next refresh appeared to reintroduce the
        // very background the user had just removed.
        {
            const QSignalBlocker typeBlocker(backgroundType);
            const int solidIndex = backgroundType->findData(QStringLiteral("solid"));
            if (solidIndex >= 0) backgroundType->setCurrentIndex(solidIndex);
            const QSignalBlocker fillBlocker(backgroundFillMode);
            const int coverIndex = backgroundFillMode->findData(QStringLiteral("cover"));
            if (coverIndex >= 0) backgroundFillMode->setCurrentIndex(coverIndex);
        }
        backgroundImage->clear();
        const QSet<QString> globalColorKeys = {
            QStringLiteral("appearance/backgroundColor"), QStringLiteral("appearance/backgroundSecondary"),
            QStringLiteral("appearance/panelColor"), QStringLiteral("appearance/toolbarColor"),
            QStringLiteral("appearance/timelineColor"), QStringLiteral("appearance/textColor"),
            QStringLiteral("appearance/borderColor"), QStringLiteral("appearance/accentColor")};
        for (auto* swatch : appearanceContent->findChildren<QPushButton*>()) {
            if (!globalColorKeys.contains(swatch->property("cgplay.appearanceKey").toString())) continue;
            const QColor color(swatch->property("cgplay.appearanceFallbackColor").toString());
            swatch->setProperty("selectedColor", color.name(QColor::HexArgb));
            swatch->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;min-height:30px;}")
                .arg(color.name(QColor::HexArgb), colorButtonText(color)));
        }
        for (auto* slider : appearanceContent->findChildren<LiveSlider*>()) {
            if (!slider->property("cgplay.appearanceKey").toString().isEmpty()) {
                const QSignalBlocker blocker(slider);
                slider->setSmoothValue(slider->property("cgplay.appearanceFallback").toInt());
            }
        }
        for (auto* editor : appearanceContent->findChildren<QSpinBox*>()) {
            if (!editor->property("cgplay.appearanceKey").toString().isEmpty()) {
                const QSignalBlocker blocker(editor);
                editor->setValue(editor->property("cgplay.appearanceFallback").toInt());
            }
        }
        if (auto* dynamic = appearanceContent->findChild<QCheckBox*>(QStringLiteral("CGPlayDynamicBackgroundCheck"))) {
            const QSignalBlocker blocker(dynamic);
            dynamic->setChecked(false);
        }
        applyPlayerWindowOpacity(this, 100);
        refreshAppearanceImmediately();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("主题已恢复默认"), 2000);
    });

    auto* opacityGroup = new QGroupBox(QStringLiteral("透明度"), appearanceContent);
    auto* opacityForm = new QFormLayout(opacityGroup);
    auto* windowOpacity = new LiveSlider(Qt::Horizontal, opacityGroup);
    windowOpacity->setProperty("cgplay.appearanceKey", QStringLiteral("appearance/windowOpacity"));
    windowOpacity->setProperty("cgplay.appearanceFallback", 100);
    windowOpacity->setSmoothRange(40, 100);
    windowOpacity->setTracking(true);
    windowOpacity->setSmoothValue(_p->userSettings ? _p->userSettings->value(QStringLiteral("appearance/windowOpacity"), 100).toInt() : 100);
    auto* windowOpacityLabel = new QSpinBox(opacityGroup);
    windowOpacityLabel->setProperty("cgplay.appearanceKey", QStringLiteral("appearance/windowOpacity"));
    windowOpacityLabel->setProperty("cgplay.appearanceFallback", 100);
    windowOpacityLabel->setRange(40, 100); windowOpacityLabel->setSuffix(QStringLiteral("%")); windowOpacityLabel->setKeyboardTracking(false); windowOpacityLabel->setFixedWidth(72);
    auto updateOpacityLabel = [windowOpacityLabel, windowOpacity] { const QSignalBlocker blocker(windowOpacityLabel); windowOpacityLabel->setValue(windowOpacity->logicalValue()); };
    updateOpacityLabel();
    auto* opacityHost = new QWidget(opacityGroup); auto* opacityLayout = new QHBoxLayout(opacityHost); opacityLayout->setContentsMargins(0, 0, 0, 0); opacityLayout->addWidget(windowOpacity, 1); opacityLayout->addWidget(windowOpacityLabel);
    opacityForm->addRow(QStringLiteral("播放器窗口"), opacityHost);
    auto* windowOpacityPreviewTimer = new QTimer(opacityGroup);
    windowOpacityPreviewTimer->setSingleShot(true);
    const auto pendingWindowOpacity = std::make_shared<int>(windowOpacity->logicalValue());
    connect(windowOpacityPreviewTimer, &QTimer::timeout, this, [this, pendingWindowOpacity] {
        applyPlayerWindowOpacity(this, *pendingWindowOpacity);
    });
    auto* buttonOpacity = new LiveSlider(Qt::Horizontal, opacityGroup); buttonOpacity->setProperty("cgplay.appearanceKey", QStringLiteral("appearance/buttonOpacity")); buttonOpacity->setProperty("cgplay.appearanceFallback", 88); buttonOpacity->setSmoothRange(30, 100); buttonOpacity->setTracking(true); buttonOpacity->setSmoothValue(_p->userSettings ? _p->userSettings->value(QStringLiteral("appearance/buttonOpacity"), 88).toInt() : 88);
    auto* buttonOpacityLabel = new QSpinBox(opacityGroup); buttonOpacityLabel->setProperty("cgplay.appearanceKey", QStringLiteral("appearance/buttonOpacity")); buttonOpacityLabel->setProperty("cgplay.appearanceFallback", 88); buttonOpacityLabel->setRange(30, 100); buttonOpacityLabel->setSuffix(QStringLiteral("%")); buttonOpacityLabel->setKeyboardTracking(false); buttonOpacityLabel->setFixedWidth(72); auto updateButtonOpacity = [buttonOpacityLabel, buttonOpacity] { const QSignalBlocker blocker(buttonOpacityLabel); buttonOpacityLabel->setValue(buttonOpacity->logicalValue()); }; updateButtonOpacity();
    auto* buttonOpacityHost = new QWidget(opacityGroup); auto* buttonOpacityLayout = new QHBoxLayout(buttonOpacityHost); buttonOpacityLayout->setContentsMargins(0, 0, 0, 0); buttonOpacityLayout->addWidget(buttonOpacity, 1); buttonOpacityLayout->addWidget(buttonOpacityLabel);
    opacityForm->addRow(QStringLiteral("按键"), buttonOpacityHost);
    auto scheduleAppearanceRefresh = [activateCustomTheme] {
        activateCustomTheme();
        qApp->setProperty("cgplay.appearanceRefreshPending", true);
    };
    auto* appearancePreviewTimer = new QTimer(opacityGroup);
    appearancePreviewTimer->setSingleShot(true);
    connect(appearancePreviewTimer, &QTimer::timeout, this, [] {
        const QSet<QString> surfaceNames = {
            QStringLiteral("cgplayAppearanceHost"), QStringLiteral("cgplayTopBarSurface"),
            QStringLiteral("cgplayPlaylistSurface"), QStringLiteral("ReviewPanel"),
            QStringLiteral("cgplayViewerShell"), QStringLiteral("cgplayTimelineSurface"),
            QStringLiteral("cgplayPlaybackBarSurface"), QStringLiteral("CodexAgentWorkspace"),
            QStringLiteral("AIAgentWorkspace"), QStringLiteral("AIAgentWorkspaceDock"),
            QStringLiteral("CodexAgentWorkspaceDock")};
        for (QWidget* topLevel : qApp->topLevelWidgets()) {
            if (!topLevel) continue;
            if (surfaceNames.contains(topLevel->objectName())) topLevel->update();
            for (QWidget* widget : topLevel->findChildren<QWidget*>()) {
                if (widget && surfaceNames.contains(widget->objectName())) widget->update();
            }
        }
    });
    const auto previewAppearanceValue = [appearancePreviewTimer](const QString& key, int value) {
        const QHash<QString, QByteArray> properties = {
            {QStringLiteral("appearance/backgroundOpacity"), QByteArrayLiteral("cgplay.backgroundOpacity")},
            {QStringLiteral("appearance/buttonOpacity"), QByteArrayLiteral("cgplay.buttonOpacity")},
            {QStringLiteral("appearance/panelOpacity"), QByteArrayLiteral("cgplay.panelOpacity")},
            {QStringLiteral("appearance/toolbarOpacity"), QByteArrayLiteral("cgplay.toolbarOpacity")},
            {QStringLiteral("appearance/timelineOpacity"), QByteArrayLiteral("cgplay.timelineOpacity")},
            {QStringLiteral("appearance/subtitleOpacity"), QByteArrayLiteral("cgplay.subtitleOpacity")},
            {QStringLiteral("appearance/viewerOpacity"), QByteArrayLiteral("cgplay.viewerOpacity")},
            {QStringLiteral("appearance/brightness"), QByteArrayLiteral("cgplay.backgroundBrightness")},
            {QStringLiteral("appearance/saturation"), QByteArrayLiteral("cgplay.backgroundSaturation")},
            {QStringLiteral("appearance/blurRadius"), QByteArrayLiteral("cgplay.backgroundBlurRadius")},
            {QStringLiteral("appearance/vignette"), QByteArrayLiteral("cgplay.backgroundVignette")},
            {QStringLiteral("appearance/shadowStrength"), QByteArrayLiteral("cgplay.shadowStrength")}
        };
        const QByteArray propertyName = properties.value(key);
        if (!propertyName.isEmpty() && qApp->property(propertyName.constData()).toInt() != value) {
            qApp->setProperty(propertyName.constData(), value);
            qApp->setProperty("cgplay.appearanceRefreshPending", true);
            if (!appearancePreviewTimer->isActive()) appearancePreviewTimer->start(33);
        }
    };
    auto commitAppearanceRefresh = [activateCustomTheme] {
        activateCustomTheme();
        qApp->setProperty("cgplay.appearanceRefreshPending", true);
    };
    auto addOpacitySetting = [this, opacityGroup, opacityForm, scheduleAppearanceRefresh, commitAppearanceRefresh, previewAppearanceValue](const QString& label, const QString& key, int minimum, int fallback) {
        auto* slider = new LiveSlider(Qt::Horizontal, opacityGroup); slider->setProperty("cgplay.appearanceKey", key); slider->setProperty("cgplay.appearanceFallback", fallback); slider->setSmoothRange(minimum, 100); slider->setTracking(true); slider->setSmoothValue(_p->userSettings ? _p->userSettings->value(key, fallback).toInt() : fallback);
        auto* value = new QSpinBox(opacityGroup); value->setRange(minimum, 100); value->setSuffix(QStringLiteral("%")); value->setKeyboardTracking(false); value->setFixedWidth(72); value->setValue(slider->logicalValue());
        value->setProperty("cgplay.appearanceKey", key); value->setProperty("cgplay.appearanceFallback", fallback);
        auto* host = new QWidget(opacityGroup); auto* row = new QHBoxLayout(host); row->setContentsMargins(0, 0, 0, 0); row->addWidget(slider, 1); row->addWidget(value); opacityForm->addRow(label, host);
        connect(slider, &QSlider::valueChanged, this, [this, slider, key, value, scheduleAppearanceRefresh, previewAppearanceValue](int v) {
            const int logical = LiveSlider::logicalValue(v);
            const QSignalBlocker blocker(value);
            value->setValue(logical);
            previewAppearanceValue(key, logical);
            if (!slider->isSliderDown()) {
                if (_p->userSettings && _p->userSettings->value(key).toInt() != logical) _p->userSettings->setValue(key, logical);
                scheduleAppearanceRefresh();
            }
        });
        connect(slider, &QSlider::sliderReleased, this, [this, slider, key, commitAppearanceRefresh] {
            if (_p->userSettings) _p->userSettings->setValue(key, slider->logicalValue());
            commitAppearanceRefresh();
        });
        connect(value, qOverload<int>(&QSpinBox::valueChanged), slider, [slider](int logical) {
            slider->setSmoothValue(logical);
        });
    };
    addOpacitySetting(QStringLiteral("播放器背景"), QStringLiteral("appearance/backgroundOpacity"), 20, 100);
    addOpacitySetting(QStringLiteral("面板"), QStringLiteral("appearance/panelOpacity"), 35, 96);
    addOpacitySetting(QStringLiteral("工具栏"), QStringLiteral("appearance/toolbarOpacity"), 35, 92);
    addOpacitySetting(QStringLiteral("时间轴"), QStringLiteral("appearance/timelineOpacity"), 45, 96);
    addOpacitySetting(QStringLiteral("字幕背景"), QStringLiteral("appearance/subtitleOpacity"), 20, 86);
    addOpacitySetting(QStringLiteral("播放画面周围"), QStringLiteral("appearance/viewerOpacity"), 20, 100);
    appearanceLayout->addWidget(opacityGroup);
    connect(windowOpacity, &QSlider::valueChanged, this, [this, windowOpacity, windowOpacityPreviewTimer, pendingWindowOpacity, updateOpacityLabel, activateCustomTheme, scheduleAppearanceRefresh](int value) {
        updateOpacityLabel();
        activateCustomTheme();
        const int logical = LiveSlider::logicalValue(value);
        *pendingWindowOpacity = logical;
        qApp->setProperty("cgplay.appearanceRefreshPending", true);
        if (!windowOpacityPreviewTimer->isActive()) windowOpacityPreviewTimer->start(16);
        if (!windowOpacity->isSliderDown()) {
            if (_p->userSettings && _p->userSettings->value(QStringLiteral("appearance/windowOpacity")).toInt() != logical) _p->userSettings->setValue(QStringLiteral("appearance/windowOpacity"), logical);
            scheduleAppearanceRefresh();
        }
    });
    connect(windowOpacity, &QSlider::sliderReleased, this, [this, windowOpacity, windowOpacityPreviewTimer, commitAppearanceRefresh] {
        const int logical = windowOpacity->logicalValue();
        windowOpacityPreviewTimer->stop();
        applyPlayerWindowOpacity(this, logical);
        if (_p->userSettings) _p->userSettings->setValue(QStringLiteral("appearance/windowOpacity"), logical);
        commitAppearanceRefresh();
    });
    connect(windowOpacityLabel, qOverload<int>(&QSpinBox::valueChanged), windowOpacity, [windowOpacity](int logical) {
        windowOpacity->setSmoothValue(logical);
    });
    connect(buttonOpacity, &QSlider::valueChanged, this, [this, buttonOpacity, updateButtonOpacity, scheduleAppearanceRefresh, previewAppearanceValue](int value) {
        updateButtonOpacity();
        const int logical = LiveSlider::logicalValue(value);
        previewAppearanceValue(QStringLiteral("appearance/buttonOpacity"), logical);
        if (!buttonOpacity->isSliderDown()) {
            if (_p->userSettings && _p->userSettings->value(QStringLiteral("appearance/buttonOpacity")).toInt() != logical) _p->userSettings->setValue(QStringLiteral("appearance/buttonOpacity"), logical);
            scheduleAppearanceRefresh();
        }
    });
    connect(buttonOpacity, &QSlider::sliderReleased, this, [this, buttonOpacity, commitAppearanceRefresh] {
        if (_p->userSettings) _p->userSettings->setValue(QStringLiteral("appearance/buttonOpacity"), buttonOpacity->logicalValue());
        commitAppearanceRefresh();
    });
    connect(buttonOpacityLabel, qOverload<int>(&QSpinBox::valueChanged), buttonOpacity, [buttonOpacity](int logical) {
        buttonOpacity->setSmoothValue(logical);
    });

    auto* effectsGroup = new QGroupBox(QStringLiteral("背景效果"), appearanceContent);
    auto* effectsForm = new QFormLayout(effectsGroup);
    auto addEffectSetting = [this, effectsGroup, effectsForm, scheduleAppearanceRefresh, commitAppearanceRefresh, previewAppearanceValue](const QString& label, const QString& key, int minimum, int maximum, int fallback, const QString& suffix) {
        auto* slider = new LiveSlider(Qt::Horizontal, effectsGroup);
        slider->setProperty("cgplay.appearanceKey", key);
        slider->setProperty("cgplay.appearanceFallback", fallback);
        slider->setSmoothRange(minimum, maximum);
        slider->setTracking(true);
        slider->setSmoothValue(_p->userSettings ? _p->userSettings->value(key, fallback).toInt() : fallback);
        auto* value = new QSpinBox(effectsGroup);
        value->setProperty("cgplay.appearanceKey", key);
        value->setProperty("cgplay.appearanceFallback", fallback);
        value->setRange(minimum, maximum);
        value->setSuffix(suffix);
        value->setKeyboardTracking(false);
        value->setFixedWidth(78);
        const auto update = [value, slider] { const QSignalBlocker blocker(value); value->setValue(slider->logicalValue()); };
        update();
        auto* host = new QWidget(effectsGroup);
        auto* row = new QHBoxLayout(host);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(slider, 1);
        row->addWidget(value);
        effectsForm->addRow(label, host);
        connect(slider, &QSlider::valueChanged, this, [this, slider, key, update, scheduleAppearanceRefresh, previewAppearanceValue](int v) {
            update();
            const int logical = LiveSlider::logicalValue(v);
            previewAppearanceValue(key, logical);
            if (!slider->isSliderDown()) {
                if (_p->userSettings && _p->userSettings->value(key).toInt() != logical) _p->userSettings->setValue(key, logical);
                scheduleAppearanceRefresh();
            }
        });
        connect(slider, &QSlider::sliderReleased, this, [this, slider, key, commitAppearanceRefresh] {
            if (_p->userSettings) _p->userSettings->setValue(key, slider->logicalValue());
            commitAppearanceRefresh();
        });
        connect(value, qOverload<int>(&QSpinBox::valueChanged), slider, [slider](int logical) {
            slider->setSmoothValue(logical);
        });
    };
    addEffectSetting(QStringLiteral("亮度"), QStringLiteral("appearance/brightness"), 0, 200, 100, QStringLiteral("%"));
    addEffectSetting(QStringLiteral("饱和度"), QStringLiteral("appearance/saturation"), 0, 200, 100, QStringLiteral("%"));
    addEffectSetting(QStringLiteral("背景模糊"), QStringLiteral("appearance/blurRadius"), 0, 64, 0, QStringLiteral(" px"));
    addEffectSetting(QStringLiteral("暗角"), QStringLiteral("appearance/vignette"), 0, 100, 0, QStringLiteral("%"));
    addEffectSetting(QStringLiteral("全局阴影"), QStringLiteral("appearance/shadowStrength"), 0, 100, 25, QStringLiteral("%"));
    auto* dynamicBackground = new QCheckBox(QStringLiteral("播放时动态背景"), effectsGroup);
    dynamicBackground->setObjectName(QStringLiteral("CGPlayDynamicBackgroundCheck"));
    dynamicBackground->setChecked(_p->userSettings ? _p->userSettings->value(QStringLiteral("appearance/dynamicBackground"), false).toBool() : false);
    effectsForm->addRow(QString(), dynamicBackground);
    connect(dynamicBackground, &QCheckBox::toggled, this, [this, activateCustomTheme, refreshAppearanceImmediately](bool enabled) {
        activateCustomTheme();
        if (_p->userSettings) _p->userSettings->setValue(QStringLiteral("appearance/dynamicBackground"), enabled);
        refreshAppearanceImmediately();
    });
    appearanceLayout->addWidget(effectsGroup);

    // Per-button styling is intentionally hidden from the settings UI.  The
    // Legacy per-button profile keys remain readable, but the duplicate editor is removed.
    QWidget* layoutContent = nullptr;
    QVBoxLayout* layoutPageLayout = nullptr;
    auto* layoutTab = makeSettingsPage(&layoutContent, &layoutPageLayout);
    auto* layoutGroup = new QGroupBox(QStringLiteral("工作区排布"), layoutContent);
    auto* layoutForm = new QFormLayout(layoutGroup);
    auto* layoutPreset = new QComboBox(layoutGroup);
    layoutPreset->addItems({QStringLiteral("默认审片"), QStringLiteral("全屏观看"),
                            QStringLiteral("字幕翻译"), QStringLiteral("Codex 工作台"),
                            QStringLiteral("双屏审片"), QStringLiteral("自定义布局")});
    const QString savedPreset = _p->userSettings ? _p->userSettings->value(QStringLiteral("workspace/preset"), QStringLiteral("默认审片")).toString() : QStringLiteral("默认审片");
    layoutPreset->setCurrentText(savedPreset); layoutForm->addRow(QStringLiteral("布局预设"), layoutPreset);
    auto* savePreset = new QPushButton(QStringLiteral("保存当前布局为预设"), layoutGroup); layoutForm->addRow(QString(), savePreset);
    layoutPageLayout->addWidget(layoutGroup);
    const auto workspaceFile = [](const QString& preset) {
        const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("workspaces"));
        return QDir(dir).filePath(safeWorkspaceName(preset) + QStringLiteral(".json"));
    };
    const auto constrainWorkspaceSizes = [this](QList<int> sizes) {
        if (!_p->horzSplitter || sizes.size() != _p->horzSplitter->count() || sizes.size() < 3) return sizes;
        sizes[0] = 0;
        const int leftIndex = 1;
        const int centerIndex = 2;
        const int rightIndex = sizes.size() > 3 ? sizes.size() - 1 : -1;
        int left = (_p->playlist && _p->leftVisible) ? qBound(1, sizes[leftIndex], 520) : 0;
        int right = (rightIndex >= 0 && _p->reviewPanel && _p->rightVisible) ? qBound(1, sizes[rightIndex], 520) : 0;
        const int total = qMax(_p->horzSplitter->width(), 0);
        constexpr int minimumViewerWidth = 280;
        int budget = qMax(0, total - minimumViewerWidth);
        while (left + right > budget && (left > 1 || right > 1)) {
            if (left >= right && left > 1) --left;
            else if (right > 1) --right;
            else break;
        }
        sizes[leftIndex] = left;
        if (rightIndex >= 0) sizes[rightIndex] = right;
        sizes[centerIndex] = qMax(minimumViewerWidth, total - left - right);
        return sizes;
    };
    const auto applyWorkspaceExtras = [this](const QJsonObject& object) {
        const QJsonObject codex = object.value(QStringLiteral("panels")).toObject()
            .value(QStringLiteral("codex")).toObject();
        if (codex.contains(QStringLiteral("visible"))) {
            if (auto* dock = findChild<QDockWidget*>(QStringLiteral("CodexAgentWorkspaceDock"))) {
                dock->setVisible(codex.value(QStringLiteral("visible")).toBool());
            }
        }
        if (object.contains(QStringLiteral("translationEnabled")) && _p->translationToggleAction) {
            _p->translationToggleAction->setChecked(object.value(QStringLiteral("translationEnabled")).toBool());
        }
        if (object.contains(QStringLiteral("secondaryWindow"))) {
            const bool requested = object.value(QStringLiteral("secondaryWindow")).toBool();
            if (auto* app = qobject_cast<Application*>(qApp); app && app->isAutomationMode()) return;
            QList<QPointer<SecondaryWindow>> windows;
            for (QWidget* window : qApp->topLevelWidgets()) {
                if (auto* secondary = qobject_cast<SecondaryWindow*>(window)) windows.push_back(secondary);
            }
            if (requested && windows.isEmpty()) {
                if (auto* app = qobject_cast<Application*>(qApp)) app->openNewWindow(true);
            } else if (!requested) {
                for (const auto& secondary : windows) if (secondary) secondary->close();
            }
        }
    };
    connect(layoutPreset, &QComboBox::currentTextChanged, this, [this, constrainWorkspaceSizes, applyWorkspaceExtras](const QString& preset) {
        if (_p->userSettings) {
            _p->userSettings->setValue(QStringLiteral("workspace/preset"), preset);
        }
        const QString path = _p->settingsProfileService
            ? _p->settingsProfileService->workspacePath(preset)
            : QString{};
        const bool hasSavedWorkspace = QFileInfo::exists(path) || QFileInfo::exists(path + QStringLiteral(".bak"));
        if (!hasSavedWorkspace) {
            // Built-in presets must remain deterministic even before a user
            // has saved a JSON file for them.  Previously selecting one of
            // these entries silently did nothing and left the prior layout
            // in place, which made the editor appear broken after restart.
            QDockWidget* codexDock = findChild<QDockWidget*>(QStringLiteral("CodexAgentWorkspaceDock"));
            const bool secondaryRequested = preset == QStringLiteral("双屏审片");
            const bool translationRequested = preset == QStringLiteral("字幕翻译");
            if (preset == QStringLiteral("默认审片")) {
                _p->leftVisible = true;
                _p->rightVisible = true;
                if (codexDock) codexDock->show();
            } else if (preset == QStringLiteral("双屏审片")) {
                _p->leftVisible = true;
                _p->rightVisible = true;
                if (codexDock) codexDock->hide();
            } else if (preset == QStringLiteral("全屏观看")) {
                _p->leftVisible = false;
                _p->rightVisible = false;
                if (codexDock) codexDock->hide();
            } else if (preset == QStringLiteral("Codex 工作台")) {
                _p->leftVisible = false;
                _p->rightVisible = false;
                if (codexDock) { codexDock->show(); codexDock->raise(); }
            } else if (preset == QStringLiteral("字幕翻译")) {
                _p->leftVisible = false;
                _p->rightVisible = false;
                if (codexDock) codexDock->hide();
            } else {
                return;
            }
            QJsonObject extras{{QStringLiteral("translationEnabled"), translationRequested},
                               {QStringLiteral("secondaryWindow"), secondaryRequested}};
            extras.insert(QStringLiteral("panels"), QJsonObject{
                {QStringLiteral("codex"), QJsonObject{{QStringLiteral("visible"), codexDock && codexDock->isVisible()}}}});
            applyWorkspaceExtras(extras);
            _applyAdaptiveSidePanelLayout(true);
            return;
        }
        bool recovered = false;
        QString workspaceError;
        if (!_p->workspaceController || !_p->workspaceController->load(preset, &recovered, &workspaceError)) {
            qWarning() << "[Workspace] Failed to load" << preset << workspaceError;
            return;
        }
        const WorkspaceProfile& workspace = _p->workspaceController->currentProfile();
        const auto playlistPanel = workspace.panels.value(QStringLiteral("playlist"));
        const auto reviewPanel = workspace.panels.value(QStringLiteral("review"));
        _p->leftVisible = workspace.panels.contains(QStringLiteral("playlist")) ? playlistPanel.visible : true;
        _p->rightVisible = workspace.panels.contains(QStringLiteral("review")) ? reviewPanel.visible : true;
        if (playlistPanel.size > 0) _p->lastLeftPanelWidth = playlistPanel.size;
        if (reviewPanel.size > 0) _p->lastRightPanelWidth = reviewPanel.size;
        if (preset == QStringLiteral("默认审片")) {
            _p->leftVisible = true;
            _p->rightVisible = true;
        }
        applyWorkspaceExtras(workspace.toJson());
        _applyAdaptiveSidePanelLayout(true);
        if (_p->horzSplitter && workspace.splitterSizes.size() == _p->horzSplitter->count()) {
            QList<int> restored;
            for (const int value : workspace.splitterSizes) restored.push_back(std::max(0, value));
            _p->horzSplitter->setSizes(constrainWorkspaceSizes(restored));
        }
    });
    connect(savePreset, &QPushButton::clicked, this, [this, layoutPreset] {
        const QString preset = layoutPreset->currentText();
        QJsonObject panels;
        panels.insert(QStringLiteral("playlist"), QJsonObject{{QStringLiteral("visible"), _p->leftVisible}, {QStringLiteral("area"), QStringLiteral("left")}, {QStringLiteral("size"), _p->playlist && _p->playlist->width() > 0 ? _p->playlist->width() : _p->lastLeftPanelWidth}});
        panels.insert(QStringLiteral("review"), QJsonObject{{QStringLiteral("visible"), _p->rightVisible}, {QStringLiteral("area"), QStringLiteral("right")}, {QStringLiteral("size"), _p->reviewPanel && _p->reviewPanel->width() > 0 ? _p->reviewPanel->width() : _p->lastRightPanelWidth}});
        if (auto* codexDock = findChild<QDockWidget*>(QStringLiteral("CodexAgentWorkspaceDock"))) {
            panels.insert(QStringLiteral("codex"), QJsonObject{{QStringLiteral("visible"), codexDock->isVisible()}, {QStringLiteral("area"), QStringLiteral("right")}, {QStringLiteral("size"), codexDock->width()}});
        }
        QJsonArray sizes;
        if (_p->horzSplitter) for (const int size : _p->horzSplitter->sizes()) sizes.append(size);
        bool hasSecondaryWindow = false;
        for (QWidget* window : qApp->topLevelWidgets()) if (qobject_cast<SecondaryWindow*>(window)) { hasSecondaryWindow = true; break; }
        const QJsonObject document{{QStringLiteral("version"), 2}, {QStringLiteral("name"), preset}, {QStringLiteral("panels"), panels}, {QStringLiteral("splitterSizes"), sizes},
            {QStringLiteral("translationEnabled"), _p->translationToggleAction && _p->translationToggleAction->isChecked()},
            {QStringLiteral("secondaryWindow"), hasSecondaryWindow},
            {QStringLiteral("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
        bool valid = false;
        QString workspaceError;
        const WorkspaceProfile workspace = WorkspaceProfile::fromJson(document, &valid, &workspaceError);
        if (!valid || !_p->workspaceController || !_p->workspaceController->save(workspace, &workspaceError)) {
            if (statusBar()) statusBar()->showMessage(QStringLiteral("工作区预设写入失败：%1").arg(workspaceError), 3000);
            return;
        }
        const QString path = _p->settingsProfileService->workspacePath(preset);
        if (_p->userSettings) { _p->userSettings->setValue(QStringLiteral("workspace/preset"), preset); _p->userSettings->sync(); }
        if (statusBar()) statusBar()->showMessage(QStringLiteral("工作区预设已保存：%1").arg(path), 2500);
    });

    auto* profileGroup = new QGroupBox(QStringLiteral("配置导入/导出"), layoutContent);
    auto* profileLayout = new QHBoxLayout(profileGroup);
    auto* exportProfile = new QPushButton(QStringLiteral("导出配置"), profileGroup);
    auto* importProfile = new QPushButton(QStringLiteral("导入配置"), profileGroup);
    importProfile->setObjectName(QStringLiteral("SettingsImportProfile"));
    profileLayout->addWidget(exportProfile);
    profileLayout->addWidget(importProfile);
    layoutPageLayout->addWidget(profileGroup);
    connect(exportProfile, &QPushButton::clicked, this, [this, workspaceFile, exportProfile] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 CGPlay 配置"),
            QStringLiteral("CGPlay_Workspace_Profile.zip"), QStringLiteral("配置包 (*.zip);;JSON 文件 (*.json)"));
        if (path.isEmpty()) return;
        QJsonObject theme;
        // Keep the profile self-contained: every appearance setting written by
        // the editor must survive export/import, including background assets,
        // semantic surface colors, and constrained opacity controls.
        for (const QString& key : {
            QStringLiteral("mode"), QStringLiteral("backgroundColor"), QStringLiteral("backgroundSecondary"),
            QStringLiteral("panelColor"), QStringLiteral("toolbarColor"), QStringLiteral("timelineColor"),
            QStringLiteral("textColor"), QStringLiteral("borderColor"), QStringLiteral("accentColor"),
            QStringLiteral("backgroundType"), QStringLiteral("backgroundImage"), QStringLiteral("texturePath"),
            QStringLiteral("fillMode"), QStringLiteral("backgroundOpacity"), QStringLiteral("panelOpacity"),
            QStringLiteral("toolbarOpacity"), QStringLiteral("timelineOpacity"), QStringLiteral("subtitleOpacity"),
            QStringLiteral("viewerOpacity"), QStringLiteral("vignette"), QStringLiteral("blurRadius"),
            QStringLiteral("brightness"), QStringLiteral("saturation"), QStringLiteral("shadowStrength"),
            QStringLiteral("dynamicBackground"), QStringLiteral("windowOpacity"),
            QStringLiteral("buttonOpacity")}) {
            if (_p->userSettings) theme.insert(key, QJsonValue::fromVariant(_p->userSettings->value(QStringLiteral("appearance/") + key)));
        }
        QJsonObject shortcuts;
        for (const auto& descriptor : _p->commandDescriptors) {
            const QString key = QStringLiteral("shortcuts/%1").arg(descriptor.id);
            if (_p->userSettings && _p->userSettings->contains(key)) shortcuts.insert(descriptor.id, _p->userSettings->value(key).toString());
        }
        QJsonObject toolbar;
        for (const auto& contribution : _p->toolbarContributions) {
            const QString visibleKey = QStringLiteral("toolbar/%1/visible").arg(contribution.id);
            const QString orderKey = QStringLiteral("toolbar/%1/order").arg(contribution.id);
            if (_p->userSettings && (_p->userSettings->contains(visibleKey) || _p->userSettings->contains(orderKey))) {
                toolbar.insert(contribution.id, QJsonObject{
                    {QStringLiteral("visible"), _p->userSettings->value(visibleKey, true).toBool()},
                    {QStringLiteral("order"), _p->userSettings->value(orderKey, contribution.order).toInt()}
                });
            }
        }
        if (_p->userSettings) {
            QJsonParseError customError{};
            const QJsonDocument customDoc = QJsonDocument::fromJson(_p->userSettings->value(QStringLiteral("toolbar/customButtons")).toByteArray(), &customError);
            if (customError.error == QJsonParseError::NoError && customDoc.isArray()) toolbar.insert(QStringLiteral("customButtons"), customDoc.array());
        }
        QJsonObject buttonStyles;
        QSet<QString> buttonIds;
        for (const auto& descriptor : _p->commandDescriptors) buttonIds.insert(descriptor.id);
        for (QAbstractButton* button : findChildren<QAbstractButton*>()) {
            if (!button) continue;
            QString styleId = button->property("commandId").toString().trimmed();
            if (styleId.isEmpty()) styleId = button->property("cgplay.style.id").toString().trimmed();
            if (!styleId.isEmpty()) buttonIds.insert(styleId);
        }
        if (_p->userSettings) {
            QJsonParseError customStyleError{};
            const QJsonDocument customStyleDocument = QJsonDocument::fromJson(
                _p->userSettings->value(QStringLiteral("toolbar/customButtons")).toByteArray(),
                &customStyleError);
            if (customStyleError.error == QJsonParseError::NoError && customStyleDocument.isArray()) {
                for (const QJsonValue& value : customStyleDocument.array()) {
                    const QString id = value.toObject().value(QStringLiteral("id")).toString().trimmed();
                    if (!id.isEmpty()) buttonIds.insert(id);
                }
            }
        }
        for (const QString& id : buttonIds) {
            QJsonObject style;
            for (const QString& field : {QStringLiteral("buttonColor"), QStringLiteral("buttonHover"), QStringLiteral("buttonPressed"), QStringLiteral("buttonDisabled"), QStringLiteral("buttonText"), QStringLiteral("buttonIcon"), QStringLiteral("buttonBorder")}) {
                const QString key = QStringLiteral("appearance/%1/%2").arg(field, id);
                if (_p->userSettings && _p->userSettings->contains(key)) style.insert(field, _p->userSettings->value(key).toString());
            }
            for (const QString& field : {QStringLiteral("buttonRadius"), QStringLiteral("buttonIconSize"), QStringLiteral("buttonOpacity"), QStringLiteral("buttonBorderOpacity"), QStringLiteral("buttonShadow"), QStringLiteral("buttonWidth"), QStringLiteral("buttonHeight"), QStringLiteral("buttonShowText"), QStringLiteral("buttonShowIcon")}) {
                const QString key = QStringLiteral("appearance/%1/%2").arg(field, id);
                if (_p->userSettings && _p->userSettings->contains(key)) style.insert(field, QJsonValue::fromVariant(_p->userSettings->value(key)));
            }
            if (!style.isEmpty()) buttonStyles.insert(id, style);
        }
        const auto readBindingObject = [](const QString& fileName) {
            QFile file(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(fileName));
            if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
            return error.error == QJsonParseError::NoError && document.isObject()
                ? document.object() : QJsonObject{};
        };
        const QJsonObject mouseBindings = readBindingObject(QStringLiteral("mouse_bindings.json"));
        const QJsonObject gamepadBindings = readBindingObject(QStringLiteral("gamepad_bindings.json"));
        const QJsonObject input{{QStringLiteral("mouseWheel"), _p->userSettings ? _p->userSettings->value(QStringLiteral("input/mouseWheel"), QStringLiteral("zoom")).toString() : QStringLiteral("zoom")}};
        const QString workspacePreset = _p->userSettings
            ? _p->userSettings->value(QStringLiteral("workspace/preset"), QStringLiteral("默认审片")).toString()
            : QStringLiteral("默认审片");
        QJsonObject workspacePanels;
        workspacePanels.insert(QStringLiteral("playlist"), QJsonObject{
            {QStringLiteral("visible"), _p->leftVisible}, {QStringLiteral("area"), QStringLiteral("left")},
            {QStringLiteral("size"), _p->playlist && _p->playlist->width() > 0 ? _p->playlist->width() : _p->lastLeftPanelWidth}});
        workspacePanels.insert(QStringLiteral("review"), QJsonObject{
            {QStringLiteral("visible"), _p->rightVisible}, {QStringLiteral("area"), QStringLiteral("right")},
            {QStringLiteral("size"), _p->reviewPanel && _p->reviewPanel->width() > 0 ? _p->reviewPanel->width() : _p->lastRightPanelWidth}});
        if (auto* codexDock = findChild<QDockWidget*>(QStringLiteral("CodexAgentWorkspaceDock"))) {
            workspacePanels.insert(QStringLiteral("codex"), QJsonObject{
                {QStringLiteral("visible"), codexDock->isVisible()}, {QStringLiteral("area"), QStringLiteral("right")},
                {QStringLiteral("size"), codexDock->width()}});
        }
        QJsonArray workspaceSizes;
        if (_p->horzSplitter) for (int size : _p->horzSplitter->sizes()) workspaceSizes.append(size);
        bool hasSecondaryWindow = false;
        for (QWidget* window : qApp->topLevelWidgets()) if (qobject_cast<SecondaryWindow*>(window)) { hasSecondaryWindow = true; break; }
        const QJsonObject workspace{{QStringLiteral("version"), 2}, {QStringLiteral("name"), workspacePreset},
            {QStringLiteral("panels"), workspacePanels}, {QStringLiteral("splitterSizes"), workspaceSizes},
            {QStringLiteral("translationEnabled"), _p->translationToggleAction && _p->translationToggleAction->isChecked()},
            {QStringLiteral("secondaryWindow"), hasSecondaryWindow}};
        QJsonObject profile{{QStringLiteral("version"), 3}, {QStringLiteral("theme"), theme}, {QStringLiteral("shortcuts"), shortcuts}, {QStringLiteral("toolbar"), toolbar}, {QStringLiteral("buttonStyles"), buttonStyles}, {QStringLiteral("input"), input}, {QStringLiteral("mouseBindings"), mouseBindings}, {QStringLiteral("gamepadBindings"), gamepadBindings}, {QStringLiteral("workspace"), workspace}, {QStringLiteral("workspacePreset"), workspacePreset}};
        if (path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
            const QString staging = QDir(QDir::tempPath()).filePath(QStringLiteral("cgplay-profile-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QDir().mkpath(staging);
            bool ok = true;
            QJsonObject packagedTheme = theme;
            const auto packageThemeAsset = [&staging, &packagedTheme, &ok](const QString& key, const QString& baseName) {
                const QFileInfo source(packagedTheme.value(key).toString());
                if (source.filePath().trimmed().isEmpty() || !source.isFile()) return;
                const QString suffix = source.suffix().isEmpty() ? QStringLiteral("bin") : source.suffix().toLower();
                const QString relative = QStringLiteral("assets/%1.%2").arg(baseName, suffix);
                const QString destination = QDir(staging).filePath(relative);
                if (!QDir().mkpath(QFileInfo(destination).absolutePath()) ||
                    (!QFile::copy(source.absoluteFilePath(), destination) && !QFileInfo::exists(destination))) {
                    ok = false;
                    return;
                }
                packagedTheme.insert(key, relative);
            };
            packageThemeAsset(QStringLiteral("backgroundImage"), QStringLiteral("background"));
            packageThemeAsset(QStringLiteral("texturePath"), QStringLiteral("texture"));
            const QList<QPair<QString, QJsonObject>> parts = {{QStringLiteral("theme.json"), packagedTheme}, {QStringLiteral("shortcuts.json"), shortcuts}, {QStringLiteral("toolbar.json"), toolbar}, {QStringLiteral("button_styles.json"), buttonStyles}, {QStringLiteral("input.json"), input}, {QStringLiteral("mouse_bindings.json"), mouseBindings}, {QStringLiteral("gamepad_bindings.json"), gamepadBindings}, {QStringLiteral("workspace.json"), workspace}, {QStringLiteral("metadata.json"), QJsonObject{{QStringLiteral("version"), 3}, {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}}};
            for (const auto& part : parts) { QSaveFile out(QDir(staging).filePath(part.first)); if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(part.second).toJson(QJsonDocument::Indented)) < 0 || !out.commit()) { ok = false; break; } }
            QString safeStage = staging; safeStage.replace("'", "''"); QString safePath = QFileInfo(path).absoluteFilePath(); safePath.replace("'", "''");
            if (ok) {
                const QPointer<QPushButton> exportGuard(exportProfile);
                exportProfile->setEnabled(false);
                ok = runProcessResponsive(QStringLiteral("powershell.exe"),
                    {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                     QStringLiteral("Compress-Archive -Path '%1\\*' -DestinationPath '%2' -Force").arg(safeStage, safePath)},
                    120000) && QFileInfo::exists(path);
                if (exportGuard) exportGuard->setEnabled(true);
            }
            QDir(staging).removeRecursively();
            if (!ok) { if (statusBar()) statusBar()->showMessage(QStringLiteral("配置包导出失败"), 2500); return; }
            if (statusBar()) statusBar()->showMessage(QStringLiteral("配置包已导出：%1").arg(path), 2500);
            return;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(profile).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
            if (statusBar()) statusBar()->showMessage(QStringLiteral("配置导出失败"), 2500);
            return;
        }
        if (statusBar()) statusBar()->showMessage(QStringLiteral("配置已导出：%1").arg(path), 2500);
    });
    connect(importProfile, &QPushButton::clicked, this, [this, importProfile, refreshShortcutUi] {
        const bool isolatedAutomation = qApp->property("cgplay.automationBackground").toBool() && _p->userSettings &&
            _p->userSettings->application().startsWith(QStringLiteral("CGPlayAutomation_"));
        const QString path = isolatedAutomation
            ? qApp->property("cgplay.automationSettingsImportPath").toString()
            : QFileDialog::getOpenFileName(this, QStringLiteral("导入 CGPlay 配置"), {}, QStringLiteral("配置包 (*.zip);;JSON 文件 (*.json)"));
        if (path.isEmpty() || !_p->userSettings) return;
        QString inputPath = path;
        QString extracted;
        if (path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
            extracted = QDir(QDir::tempPath()).filePath(QStringLiteral("cgplay-import-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QDir().mkpath(extracted); QString safeZip = QFileInfo(path).absoluteFilePath(); safeZip.replace("'", "''"); QString safeOut = extracted; safeOut.replace("'", "''");
            const QPointer<QPushButton> importGuard(importProfile);
            importProfile->setEnabled(false);
            const bool unzipped = runProcessResponsive(QStringLiteral("powershell.exe"),
                {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                 QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force").arg(safeZip, safeOut)},
                120000);
            if (importGuard) importGuard->setEnabled(true);
            if (!unzipped) { QDir(extracted).removeRecursively(); if (statusBar()) statusBar()->showMessage(QStringLiteral("配置包解压失败"), 2500); return; }
            inputPath = QDir(extracted).filePath(QStringLiteral("profile.json"));
            QJsonObject merged{{QStringLiteral("version"), 3}};
            const QStringList names = {
                QStringLiteral("theme.json"), QStringLiteral("shortcuts.json"),
                QStringLiteral("toolbar.json"), QStringLiteral("button_styles.json"),
                QStringLiteral("input.json"), QStringLiteral("mouse_bindings.json"),
                QStringLiteral("gamepad_bindings.json"), QStringLiteral("workspace.json")};
            for (const QString& name : names) {
                QFile part(QDir(extracted).filePath(name));
                if (!part.open(QIODevice::ReadOnly)) continue;
                QJsonParseError partError{};
                const QJsonDocument partDocument = QJsonDocument::fromJson(part.readAll(), &partError);
                if (partError.error != QJsonParseError::NoError || !partDocument.isObject()) continue;
                if (name == QStringLiteral("workspace.json")) {
                    const QJsonObject workspace = partDocument.object();
                    merged.insert(QStringLiteral("workspace"), workspace);
                    merged.insert(QStringLiteral("workspacePreset"),
                        workspace.value(QStringLiteral("name")).toString(
                            workspace.value(QStringLiteral("preset")).toString()));
                } else if (name == QStringLiteral("button_styles.json")) {
                    merged.insert(QStringLiteral("buttonStyles"), partDocument.object());
                } else if (name == QStringLiteral("mouse_bindings.json")) {
                    merged.insert(QStringLiteral("mouseBindings"), partDocument.object());
                } else if (name == QStringLiteral("gamepad_bindings.json")) {
                    merged.insert(QStringLiteral("gamepadBindings"), partDocument.object());
                } else {
                    merged.insert(name.left(name.size() - 5), partDocument.object());
                }
            }
            QSaveFile assembled(inputPath); if (!assembled.open(QIODevice::WriteOnly) || assembled.write(QJsonDocument(merged).toJson()) < 0 || !assembled.commit()) { QDir(extracted).removeRecursively(); return; }
        }
        QFile file(inputPath);
        if (!file.open(QIODevice::ReadOnly)) return;
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) { if (!extracted.isEmpty()) QDir(extracted).removeRecursively(); if (statusBar()) statusBar()->showMessage(QStringLiteral("配置文件无效"), 2500); return; }
        QJsonObject profile = document.object();
        QSet<QString> knownCommands;
        for (const auto& descriptor : _p->commandDescriptors) {
            if (!descriptor.id.trimmed().isEmpty()) knownCommands.insert(descriptor.id.trimmed());
        }
        const auto rejectProfile = [this, &extracted](const QString& message) {
            if (!extracted.isEmpty()) QDir(extracted).removeRecursively();
            if (statusBar()) statusBar()->showMessage(message, 3500);
        };
        QString validationError;
        bool packageValid = false;
        const SettingsProfilePackage typedPackage = SettingsProfilePackage::fromJson(
            profile, knownCommands, &packageValid, &validationError);
        if (!packageValid) {
            rejectProfile(QStringLiteral("配置校验失败：%1").arg(validationError));
            return;
        }
        profile = typedPackage.toJson();
        const QJsonObject shortcuts = typedPackage.shortcuts;
        const QJsonObject toolbar = typedPackage.toolbar.toJson();
        const QJsonObject input = typedPackage.input;
        const QJsonObject importedMouseBindings = typedPackage.mouseBindings;
        const QJsonObject importedGamepadBindings = typedPackage.gamepadBindings;
        const QJsonObject importedWorkspace = typedPackage.workspace.toJson();
        const QString importedPreset = safeWorkspaceName(typedPackage.workspacePreset);

        QJsonObject theme = profile.value(QStringLiteral("theme")).toObject();
        if (!extracted.isEmpty()) {
            const QString extractedRoot = QFileInfo(extracted).canonicalFilePath();
            const QString assetRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("themes/assets"));
            for (const QString& key : {QStringLiteral("backgroundImage"), QStringLiteral("texturePath")}) {
                const QString configured = theme.value(key).toString().trimmed();
                if (configured.isEmpty() || QFileInfo(configured).isAbsolute()) continue;
                const QFileInfo source(QDir(extracted).filePath(configured));
                const QString canonicalSource = source.canonicalFilePath();
                if (!source.isFile() || canonicalSource.isEmpty() ||
                    (!extractedRoot.isEmpty() && !canonicalSource.startsWith(extractedRoot + QDir::separator(), Qt::CaseInsensitive))) {
                    rejectProfile(QStringLiteral("配置包背景资源无效：%1").arg(configured));
                    return;
                }
                if (!QDir().mkpath(assetRoot)) {
                    rejectProfile(QStringLiteral("无法创建主题资源目录"));
                    return;
                }
                const QString destination = QDir(assetRoot).filePath(
                    QStringLiteral("%1_%2.%3")
                        .arg(key, QUuid::createUuid().toString(QUuid::WithoutBraces),
                             source.suffix().isEmpty() ? QStringLiteral("bin") : source.suffix().toLower()));
                QFile sourceFile(canonicalSource);
                QSaveFile destinationFile(destination);
                if (!sourceFile.open(QIODevice::ReadOnly) || !destinationFile.open(QIODevice::WriteOnly) ||
                    destinationFile.write(sourceFile.readAll()) < 0 || !destinationFile.commit()) {
                    rejectProfile(QStringLiteral("配置包背景资源复制失败"));
                    return;
                }
                theme.insert(key, destination);
            }
        }
        const QJsonObject buttonStyles = profile.value(QStringLiteral("buttonStyles")).toObject();
        const auto writeBindingObject = [](const QString& fileName, const QJsonObject& object) {
            if (object.isEmpty()) return true;
            const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (!QDir().mkpath(root)) return false;
            const QString path = QDir(root).filePath(fileName);
            if (QFile::exists(path)) {
                const QString backupPath = path + QStringLiteral(".bak");
                QFile::remove(backupPath);
                if (!QFile::copy(path, backupPath)) return false;
            }
            QSaveFile file(path);
            return file.open(QIODevice::WriteOnly) &&
                file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) >= 0 &&
                file.commit();
        };
        if (!importedWorkspace.isEmpty()) {
            QJsonObject workspaceToWrite = importedWorkspace;
            workspaceToWrite.insert(QStringLiteral("name"), importedPreset);
            const QString workspaceDir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("workspaces"));
            if (!QDir().mkpath(workspaceDir)) {
                if (!extracted.isEmpty()) QDir(extracted).removeRecursively();
                if (statusBar()) statusBar()->showMessage(QStringLiteral("无法创建工作区目录"), 3000);
                return;
            }
            const QString workspacePath = QDir(workspaceDir).filePath(importedPreset + QStringLiteral(".json"));
            if (QFile::exists(workspacePath)) {
                QFile::remove(workspacePath + QStringLiteral(".bak"));
                QFile::copy(workspacePath, workspacePath + QStringLiteral(".bak"));
            }
            QSaveFile workspaceFile(workspacePath);
            if (!workspaceFile.open(QIODevice::WriteOnly) ||
                workspaceFile.write(QJsonDocument(workspaceToWrite).toJson(QJsonDocument::Indented)) < 0 ||
                !workspaceFile.commit()) {
                if (!extracted.isEmpty()) QDir(extracted).removeRecursively();
                if (statusBar()) statusBar()->showMessage(QStringLiteral("工作区导入失败"), 3000);
                return;
            }
        }
        if (!writeBindingObject(QStringLiteral("mouse_bindings.json"), importedMouseBindings) ||
            !writeBindingObject(QStringLiteral("gamepad_bindings.json"), importedGamepadBindings)) {
            if (!extracted.isEmpty()) QDir(extracted).removeRecursively();
            if (statusBar()) statusBar()->showMessage(QStringLiteral("输入绑定导入失败"), 3000);
            return;
        }

        // Persistent settings are applied only after every file-backed part
        // has validated and committed, preventing a rejected workspace from
        // leaving a partially imported theme or shortcut set.
        for (auto it = theme.begin(); it != theme.end(); ++it) _p->userSettings->setValue(QStringLiteral("appearance/") + it.key(), it.value().toVariant());
        for (auto it = shortcuts.begin(); it != shortcuts.end(); ++it) _p->userSettings->setValue(QStringLiteral("shortcuts/") + it.key(), it.value().toString());
        for (auto it = toolbar.begin(); it != toolbar.end(); ++it) {
            if (it.key() == QStringLiteral("customButtons")) {
                if (it.value().isArray()) _p->userSettings->setValue(QStringLiteral("toolbar/customButtons"), QString::fromUtf8(QJsonDocument(it.value().toArray()).toJson(QJsonDocument::Compact)));
                continue;
            }
            if (it.value().isObject()) {
                const QJsonObject entry = it.value().toObject();
                _p->userSettings->setValue(
                    QStringLiteral("toolbar/") + it.key() + QStringLiteral("/visible"),
                    entry.value(QStringLiteral("visible")).toBool(true));
                if (entry.contains(QStringLiteral("order"))) {
                    _p->userSettings->setValue(
                        QStringLiteral("toolbar/") + it.key() + QStringLiteral("/order"),
                        entry.value(QStringLiteral("order")).toInt());
                }
            } else {
                _p->userSettings->setValue(QStringLiteral("toolbar/") + it.key() + QStringLiteral("/visible"), it.value().toBool());
            }
        }
        for (auto buttonIt = buttonStyles.begin(); buttonIt != buttonStyles.end(); ++buttonIt) {
            const QJsonObject style = buttonIt.value().toObject();
            for (auto styleIt = style.begin(); styleIt != style.end(); ++styleIt) _p->userSettings->setValue(QStringLiteral("appearance/") + styleIt.key() + QStringLiteral("/") + buttonIt.key(), styleIt.value().toVariant());
        }
        if (input.contains(QStringLiteral("mouseWheel"))) {
            _p->userSettings->setValue(QStringLiteral("input/mouseWheel"), input.value(QStringLiteral("mouseWheel")).toString());
        }
        if (profile.contains(QStringLiteral("workspacePreset")) || !importedWorkspace.isEmpty()) {
            _p->userSettings->setValue(QStringLiteral("workspace/preset"), importedPreset);
        }
        _p->userSettings->sync();
        if (_p->inputBindings) {
            QString inputError;
            if (!_p->inputBindings->load(&inputError)) {
                if (!extracted.isEmpty()) QDir(extracted).removeRecursively();
                if (statusBar()) statusBar()->showMessage(QStringLiteral("输入绑定刷新失败：%1").arg(inputError), 3000);
                return;
            }
        }
        if (!extracted.isEmpty()) QDir(extracted).removeRecursively();
        for (auto& descriptor : _p->commandDescriptors) {
            if (shortcuts.contains(descriptor.id)) descriptor.shortcut = shortcuts.value(descriptor.id).toString();
        }
        for (QAction* action : findChildren<QAction*>()) {
            if (!action) continue;
            const QString commandId = action->property("commandId").toString().trimmed();
            if (commandId.isEmpty() || !shortcuts.contains(commandId)) continue;
            action->setShortcut(QKeySequence(shortcuts.value(commandId).toString()));
        }
        if (_p->annoToolbar && !_p->toolbarContributions.isEmpty()) {
            const QString id = _p->toolbarContributions.constFirst().id;
            const bool visible = _p->userSettings->value(
                QStringLiteral("toolbar/%1/visible").arg(id), true).toBool();
            _p->annoToolbar->setVisible(visible);
            _p->annoToolsVisible = visible;
        }
        _rebuildCustomToolbar();
        refreshShortcutUi();
        _restoreSplitterLayoutIfNeeded();
        if (auto* app = qobject_cast<Application*>(qApp)) app->refreshAppearanceSettings();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("配置已导入并应用"), 3000);
    });
    appearanceLayout->addStretch();
    layoutPageLayout->addStretch();
    tabs->addTab(appearanceTab, QStringLiteral("外观与布局"));
    tabs->addTab(layoutTab, QStringLiteral("工作区布局"));

    QWidget* shortcutContent = nullptr;
    QVBoxLayout* shortcutLayout = nullptr;
    auto* shortcutTab = makeSettingsPage(&shortcutContent, &shortcutLayout);
    auto* shortcutGroup = new QGroupBox(QStringLiteral("命令快捷键"), shortcutContent);
    auto* shortcutForm = new QFormLayout(shortcutGroup);
    struct ShortcutEditor { QString id; QKeySequenceEdit* edit = nullptr; };
    auto* shortcutEditors = new QVector<ShortcutEditor>();
    for (const auto& descriptor : _p->commandDescriptors) {
        if (descriptor.id.isEmpty()) continue;
        auto* edit = new QKeySequenceEdit(shortcutGroup);
        edit->setObjectName(QStringLiteral("SettingsShortcut_") + descriptor.id);
        edit->setAccessibleName(descriptor.text);
        edit->setProperty("cgplay.settingsSearchText", descriptor.text + QLatin1Char(' ') + descriptor.id);
        edit->setToolTip(QStringLiteral("%1\n%2\n点击录入快捷键；清空后保存可取消绑定。 ").arg(descriptor.text, descriptor.id));
        edit->setClearButtonEnabled(true);
        if (!descriptor.shortcut.isEmpty()) edit->setKeySequence(QKeySequence(descriptor.shortcut));
        const auto localizeShortcutHint = [edit] {
            if (auto* line = edit->findChild<QLineEdit*>()) line->setPlaceholderText(QStringLiteral("点击设置快捷键"));
        };
        localizeShortcutHint();
        connect(edit, &QKeySequenceEdit::keySequenceChanged, edit, localizeShortcutHint);
        connect(edit, &QKeySequenceEdit::editingFinished, edit, localizeShortcutHint);
        auto* label = new QLabel(descriptor.text, shortcutGroup);
        label->setBuddy(edit);
        label->setToolTip(descriptor.id);
        shortcutForm->addRow(label, edit);
        shortcutEditors->push_back({descriptor.id, edit});
    }
    shortcutLayout->addWidget(shortcutGroup);
    auto* saveShortcuts = new QPushButton(QStringLiteral("保存快捷键"), shortcutContent);
    auto* restoreShortcuts = new QPushButton(QStringLiteral("恢复默认快捷键"), shortcutContent);
    saveShortcuts->setObjectName(QStringLiteral("SettingsSaveShortcuts"));
    restoreShortcuts->setObjectName(QStringLiteral("SettingsRestoreShortcuts"));
    auto* shortcutActions = new QHBoxLayout();
    shortcutActions->addWidget(saveShortcuts);
    shortcutActions->addWidget(restoreShortcuts);
    shortcutActions->addStretch();
    connect(saveShortcuts, &QPushButton::clicked, this, [this, shortcutEditors, refreshShortcutUi] {
        QHash<QString, QString> owners;
        for (const auto& entry : *shortcutEditors) {
            const QString sequence = entry.edit->keySequence().toString(QKeySequence::PortableText).trimmed();
            if (sequence.isEmpty()) continue;
            if (owners.contains(sequence)) {
                if (statusBar()) statusBar()->showMessage(QStringLiteral("快捷键冲突：%1 与 %2").arg(sequence, owners.value(sequence)), 3500);
                return;
            }
            owners.insert(sequence, entry.id);
        }
        for (const auto& entry : *shortcutEditors) {
            const QString key = QStringLiteral("shortcuts/%1").arg(entry.id);
            const QString sequence = entry.edit->keySequence().toString(QKeySequence::PortableText).trimmed();
            if (_p->userSettings) {
                // An explicit empty value means the shortcut stays cleared
                // across restarts.  Removing the key is reserved for restore.
                _p->userSettings->setValue(key, sequence);
            }
            for (QAction* action : findChildren<QAction*>(entry.id)) {
                action->setShortcut(entry.edit->keySequence());
            }
            for (auto& descriptor : _p->commandDescriptors) {
                if (descriptor.id == entry.id) {
                    descriptor.shortcut = sequence;
                    break;
                }
            }
        }
        if (_p->userSettings) _p->userSettings->sync();
        refreshShortcutUi();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("快捷键已保存"), 2000);
    });
    connect(restoreShortcuts, &QPushButton::clicked, this, [this, shortcutEditors, refreshShortcutUi] {
        for (const auto& entry : *shortcutEditors) {
            const QString key = QStringLiteral("shortcuts/%1").arg(entry.id);
            const QString sequence = _p->commandDefaultShortcuts.value(entry.id);
            if (_p->userSettings) _p->userSettings->remove(key);
            entry.edit->setKeySequence(QKeySequence(sequence));
            for (QAction* action : findChildren<QAction*>(entry.id)) {
                action->setShortcut(QKeySequence(sequence));
            }
            for (auto& descriptor : _p->commandDescriptors) {
                if (descriptor.id == entry.id) {
                    descriptor.shortcut = sequence;
                    break;
                }
            }
        }
        if (_p->userSettings) _p->userSettings->sync();
        refreshShortcutUi();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("快捷键已恢复默认"), 2000);
    });
    shortcutLayout->addStretch();
    auto* shortcutPage = new QWidget(tabs);
    auto* shortcutPageLayout = new QVBoxLayout(shortcutPage);
    shortcutPageLayout->setContentsMargins(0, 0, 0, 0);
    shortcutPageLayout->addWidget(shortcutTab, 1);
    shortcutPageLayout->addLayout(shortcutActions);
    tabs->addTab(shortcutPage, QStringLiteral("快捷键"));

    QWidget* toolbarContent = nullptr;
    QVBoxLayout* toolbarLayout = nullptr;
    auto* toolbarTab = makeSettingsPage(&toolbarContent, &toolbarLayout);
    auto* toolbarGroup = new QGroupBox(QStringLiteral("工具栏按键"), toolbarContent);
    auto* toolbarChecks = new QVBoxLayout(toolbarGroup);
    struct ToolbarEditor { QString id; QCheckBox* check = nullptr; QSpinBox* order = nullptr; };
    auto* toolbarEditors = new QVector<ToolbarEditor>();
    for (const auto& contribution : _p->toolbarContributions) {
        if (contribution.id.isEmpty()) continue;
        auto* row = new QWidget(toolbarGroup); auto* rowLayout = new QHBoxLayout(row); rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* check = new QCheckBox(contribution.labelOverride.isEmpty() ? contribution.id : contribution.labelOverride, row);
        check->setChecked(!_p->userSettings || _p->userSettings->value(
            QStringLiteral("toolbar/%1/visible").arg(contribution.id), true).toBool());
        auto* order = new QSpinBox(row); order->setRange(-1000, 1000); order->setValue(_p->userSettings ? _p->userSettings->value(QStringLiteral("toolbar/%1/order").arg(contribution.id), contribution.order).toInt() : contribution.order); order->setToolTip(QStringLiteral("排序序号"));
        rowLayout->addWidget(check, 1); rowLayout->addWidget(order); toolbarChecks->addWidget(row);
        toolbarEditors->push_back({contribution.id, check, order});
    }
    if (toolbarEditors->isEmpty()) toolbarChecks->addWidget(new QLabel(QStringLiteral("当前没有可配置的工具栏贡献"), toolbarGroup));
    toolbarLayout->addWidget(toolbarGroup);
    auto* saveToolbar = new QPushButton(QStringLiteral("保存工具栏配置"), toolbarContent);
    toolbarLayout->addWidget(saveToolbar);
    connect(saveToolbar, &QPushButton::clicked, this, [this, toolbarEditors] {
        for (const auto& entry : *toolbarEditors) {
            if (_p->userSettings) _p->userSettings->setValue(
                QStringLiteral("toolbar/%1/visible").arg(entry.id), entry.check->isChecked());
            if (_p->userSettings) _p->userSettings->setValue(QStringLiteral("toolbar/%1/order").arg(entry.id), entry.order->value());
        }
        if (_p->userSettings) _p->userSettings->sync();
        if (_p->annoToolbar && !toolbarEditors->isEmpty()) {
            const bool visible = toolbarEditors->first().check->isChecked();
            _p->annoToolbar->setVisible(visible);
            _p->annoToolsVisible = visible;
        }
        if (statusBar()) statusBar()->showMessage(QStringLiteral("工具栏配置已保存"), 2000);
    });

    // Custom toolbar buttons execute validated command sequences through the
    // same command IDs used by menus, shortcuts, mouse and gamepad input.
    auto* customGroup = new QGroupBox(QStringLiteral("自定义按键组"), toolbarContent);
    auto* customForm = new QFormLayout(customGroup);
    auto* customId = new QLineEdit(customGroup); customId->setPlaceholderText(QStringLiteral("custom.capture_annotate"));
    auto* customName = new QLineEdit(customGroup); customName->setPlaceholderText(QStringLiteral("截图并批注"));
    auto* customIcon = new QLineEdit(customGroup); customIcon->setPlaceholderText(QStringLiteral("camera-plus"));
    auto* customGroupName = new QLineEdit(customGroup); customGroupName->setPlaceholderText(QStringLiteral("自定义按键"));
    auto* customCommands = new QLineEdit(customGroup); customCommands->setPlaceholderText(QStringLiteral("codex.captureFrame,annotation.create"));
    customForm->addRow(QStringLiteral("按键 ID"), customId);
    customForm->addRow(QStringLiteral("名称"), customName);
    customForm->addRow(QStringLiteral("图标"), customIcon);
    customForm->addRow(QStringLiteral("按键组"), customGroupName);
    customForm->addRow(QStringLiteral("命令列表"), customCommands);
    auto* customActions = new QHBoxLayout();
    auto* saveCustom = new QPushButton(QStringLiteral("保存/更新"), customGroup);
    auto* clearCustom = new QPushButton(QStringLiteral("清空全部"), customGroup);
    customActions->addWidget(saveCustom); customActions->addWidget(clearCustom); customActions->addStretch();
    customForm->addRow(customActions);
    toolbarLayout->insertWidget(toolbarLayout->count() - 1, customGroup);
    auto readCustomButtons = [this]() {
        if (!_p->userSettings) return QJsonArray{};
        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(_p->userSettings->value(QStringLiteral("toolbar/customButtons")).toByteArray(), &error);
        return error.error == QJsonParseError::NoError && doc.isArray() ? doc.array() : QJsonArray{};
    };
    connect(saveCustom, &QPushButton::clicked, this, [this, customId, customName, customIcon, customGroupName, customCommands, readCustomButtons] {
        const QString id = customId->text().trimmed();
        QStringList commands;
        for (const QString& value : customCommands->text().split(',', Qt::SkipEmptyParts)) {
            const QString command = value.trimmed();
            if (!command.isEmpty()) commands.push_back(command);
        }
        static const QRegularExpression idPattern(QStringLiteral("^[a-z0-9_.-]+$"));
        if (!idPattern.match(id).hasMatch() || customName->text().trimmed().isEmpty() || commands.isEmpty()) {
            if (statusBar()) statusBar()->showMessage(QStringLiteral("自定义按键需要 ID、名称和至少一个命令"), 2500);
            return;
        }
        QSet<QString> known;
        for (const auto& descriptor : _p->commandDescriptors) known.insert(descriptor.id);
        for (const QString& command : commands) {
            if (!known.contains(command)) {
                if (statusBar()) statusBar()->showMessage(QStringLiteral("未知命令：%1").arg(command), 3000);
                return;
            }
        }
        QJsonArray buttons = readCustomButtons();
        QJsonArray updated;
        const QJsonObject value{{QStringLiteral("id"), id}, {QStringLiteral("name"), customName->text().trimmed()}, {QStringLiteral("icon"), customIcon->text().trimmed()}, {QStringLiteral("group"), customGroupName->text().trimmed()}, {QStringLiteral("commands"), QJsonArray::fromStringList(commands)}};
        for (const auto& entry : buttons) if (entry.toObject().value(QStringLiteral("id")).toString() != id) updated.append(entry);
        updated.append(value);
        if (_p->userSettings) { _p->userSettings->setValue(QStringLiteral("toolbar/customButtons"), QString::fromUtf8(QJsonDocument(updated).toJson(QJsonDocument::Compact))); _p->userSettings->sync(); }
        _rebuildCustomToolbar();
        if (auto* app = qobject_cast<Application*>(qApp)) app->refreshAppearanceSettings();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("自定义按键已保存：%1").arg(id), 2200);
    });
    connect(clearCustom, &QPushButton::clicked, this, [this] {
        if (_p->userSettings) { _p->userSettings->remove(QStringLiteral("toolbar/customButtons")); _p->userSettings->sync(); }
        _rebuildCustomToolbar();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("自定义按键已清空"), 2200);
    });
    toolbarLayout->addStretch();
    tabs->addTab(toolbarTab, QStringLiteral("工具栏"));

    QWidget* inputContent = nullptr;
    QVBoxLayout* inputLayout = nullptr;
    auto* inputTab = makeSettingsPage(&inputContent, &inputLayout);
    auto* mouseGroup = new QGroupBox(QStringLiteral("鼠标与手柄"), inputContent);
    auto* mouseForm = new QFormLayout(mouseGroup);
    auto* wheelAction = new QComboBox(mouseGroup);
    wheelAction->addItem(QStringLiteral("滚轮放大/缩小"), QStringLiteral("zoom"));
    wheelAction->addItem(QStringLiteral("无"), QStringLiteral("none"));
    wheelAction->addItem(QStringLiteral("滚轮上一帧/下一帧"), QStringLiteral("frames"));
    wheelAction->addItem(QStringLiteral("滚轮调整音量"), QStringLiteral("volume"));
    const int wheelIndex = wheelAction->findData(_p->userSettings ? _p->userSettings->value(QStringLiteral("input/mouseWheel"), QStringLiteral("zoom")) : QStringLiteral("zoom"));
    if (wheelIndex >= 0) wheelAction->setCurrentIndex(wheelIndex);
    mouseForm->addRow(QStringLiteral("鼠标滚轮"), wheelAction);
    auto* gamepadHint = new QLabel(QStringLiteral("手柄按钮绑定将在命令输入编辑器中复用同一命令 ID。"), mouseGroup);
    gamepadHint->setWordWrap(true);
    mouseForm->addRow(QStringLiteral("手柄"), gamepadHint);
    auto* inputStore = new InputBindingStore();
    inputStore->load();
    // All binding editors expose the same immutable choices. Sharing the
    // model avoids constructing sixteen copies while selections stay local.
    auto* inputCommandModel = new QStandardItemModel(mouseGroup);
    inputCommandModel->setObjectName(QStringLiteral("SettingsInputCommandModel"));
    auto* noCommand = new QStandardItem(QStringLiteral("无"));
    noCommand->setData(QString(), Qt::UserRole);
    inputCommandModel->appendRow(noCommand);
    for (const auto& descriptor : _p->commandDescriptors) {
        if (descriptor.id.trimmed().isEmpty()) continue;
        auto* item = new QStandardItem(QStringLiteral("%1 (%2)").arg(descriptor.text, descriptor.id));
        item->setData(descriptor.id, Qt::UserRole);
        inputCommandModel->appendRow(item);
    }
    struct DeviceBindingEditor {
        InputBindingStore::Device device;
        QString gesture;
        QComboBox* combo = nullptr;
    };
    auto* bindingEditors = new QVector<DeviceBindingEditor>();
    auto selectCommand = [inputStore](QComboBox* combo, InputBindingStore::Device device, const QString& gesture) {
        const QString current = inputStore->commandFor(device, gesture);
        const int index = combo->findData(current);
        if (index >= 0) combo->setCurrentIndex(index);
    };
    const auto addBindingEditor = [mouseGroup, mouseForm, inputCommandModel, selectCommand, bindingEditors](
                                      const QString& label, InputBindingStore::Device device, const QString& gesture) {
        auto* combo = new QComboBox(mouseGroup);
        combo->setObjectName(QStringLiteral("SettingsBinding_%1").arg(gesture));
        combo->setModel(inputCommandModel);
        selectCommand(combo, device, gesture);
        mouseForm->addRow(label, combo);
        bindingEditors->push_back({device, gesture, combo});
    };
    addBindingEditor(QStringLiteral("鼠标侧键1 (Mouse4)"), InputBindingStore::Device::Mouse, QStringLiteral("Mouse4"));
    addBindingEditor(QStringLiteral("鼠标侧键2 (Mouse5)"), InputBindingStore::Device::Mouse, QStringLiteral("Mouse5"));
    for (const auto& binding : QList<QPair<QString, QString>>{
             {QStringLiteral("手柄 A"), QStringLiteral("A")},
             {QStringLiteral("手柄 B"), QStringLiteral("B")},
             {QStringLiteral("手柄 X"), QStringLiteral("X")},
             {QStringLiteral("手柄 Y"), QStringLiteral("Y")},
             {QStringLiteral("左肩键"), QStringLiteral("LB")},
             {QStringLiteral("右肩键"), QStringLiteral("RB")},
             {QStringLiteral("返回键"), QStringLiteral("Back")},
             {QStringLiteral("开始键"), QStringLiteral("Start")},
             {QStringLiteral("左摇杆按下"), QStringLiteral("L3")},
             {QStringLiteral("右摇杆按下"), QStringLiteral("R3")},
             {QStringLiteral("方向键上"), QStringLiteral("DPadUp")},
             {QStringLiteral("方向键下"), QStringLiteral("DPadDown")},
             {QStringLiteral("方向键左"), QStringLiteral("DPadLeft")},
             {QStringLiteral("方向键右"), QStringLiteral("DPadRight")}}) {
        addBindingEditor(binding.first, InputBindingStore::Device::Gamepad, binding.second);
    }
    auto* saveInput = new QPushButton(QStringLiteral("保存鼠标与手柄绑定"), inputContent);
    connect(saveInput, &QPushButton::clicked, this, [this, inputStore, bindingEditors] {
        QString conflict;
        for (const auto& binding : *bindingEditors) {
            const QString old = inputStore->commandFor(binding.device, binding.gesture);
            if (!old.isEmpty()) inputStore->clearBinding(binding.device, old);
        }
        bool ok = true;
        for (const auto& binding : *bindingEditors) {
            if (!inputStore->setBinding(binding.device, binding.combo->currentData().toString(), binding.gesture, &conflict)) {
                ok = false;
                break;
            }
        }
        if (!ok) { if (statusBar()) statusBar()->showMessage(QStringLiteral("输入绑定冲突：%1").arg(conflict), 3500); return; }
        QString error;
        if (!inputStore->save(&error)) { if (statusBar()) statusBar()->showMessage(QStringLiteral("输入绑定保存失败：%1").arg(error), 3500); return; }
        if (_p->inputBindings) {
            QString reloadError;
            if (!_p->inputBindings->load(&reloadError) && statusBar()) {
                statusBar()->showMessage(QStringLiteral("输入绑定已保存但刷新失败：%1").arg(reloadError), 3500);
                return;
            }
        }
        if (statusBar()) statusBar()->showMessage(QStringLiteral("鼠标与手柄绑定已保存"), 2000);
    });
    inputLayout->addWidget(saveInput);
    inputLayout->addWidget(mouseGroup);
    connect(wheelAction, &QComboBox::currentIndexChanged, this, [this, wheelAction](int) { if (_p->userSettings) _p->userSettings->setValue(QStringLiteral("input/mouseWheel"), wheelAction->currentData().toString()); if (statusBar()) statusBar()->showMessage(QStringLiteral("鼠标滚轮绑定已保存"), 1800); });
    inputLayout->addStretch();
    tabs->addTab(inputTab, QStringLiteral("鼠标与手柄"));

    QWidget* commandContent = nullptr;
    QVBoxLayout* commandLayout = nullptr;
    auto* commandTab = makeSettingsPage(&commandContent, &commandLayout);
    auto* commandGroup = new QGroupBox(QStringLiteral("已注册命令"), commandContent);
    auto* commandBox = new QVBoxLayout(commandGroup);
    auto* commandTable = new QTableWidget(commandGroup);
    commandTable->setObjectName(QStringLiteral("SettingsCommandTable"));
    commandTable->setAccessibleName(QStringLiteral("全部命令与当前快捷键"));
    commandTable->setMinimumHeight(350);
    commandTable->setColumnCount(4);
    commandTable->setHorizontalHeaderLabels({QStringLiteral("命令 ID"), QStringLiteral("名称"), QStringLiteral("快捷键"), QStringLiteral("状态")});
    commandTable->setRowCount(_p->commandDescriptors.size());
    commandTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    commandTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    commandTable->horizontalHeader()->setStretchLastSection(false);
    commandTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    commandTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    commandTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    commandTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    commandTable->setColumnWidth(0, 210);
    commandTable->setAlternatingRowColors(true);
    for (int row = 0; row < _p->commandDescriptors.size(); ++row) {
        const auto& descriptor = _p->commandDescriptors.at(row);
        commandTable->setItem(row, 0, new QTableWidgetItem(descriptor.id));
        commandTable->setItem(row, 1, new QTableWidgetItem(descriptor.text));
        commandTable->setItem(row, 2, new QTableWidgetItem(descriptor.shortcut));
        const bool available = !descriptor.isEnabled || descriptor.isEnabled();
        commandTable->setItem(row, 3, new QTableWidgetItem(available ? QStringLiteral("可用") : QStringLiteral("不可用")));
        for (int col = 0; col < commandTable->columnCount(); ++col)
            commandTable->item(row, col)->setToolTip(commandTable->item(row, col)->text());
    }
    commandBox->addWidget(commandTable);
    commandLayout->addWidget(commandGroup);
    commandLayout->addStretch();
    tabs->addTab(commandTab, QStringLiteral("命令管理"));

    QWidget* playbackContent = nullptr;
    QVBoxLayout* playbackLayout = nullptr;
    auto* playbackTab = makeSettingsPage(&playbackContent, &playbackLayout);

    auto* audioGroup = new QGroupBox(QStringLiteral("播放与音频"), playbackContent);
    auto* audioForm = new QFormLayout(audioGroup);
    auto* volumeHost = new QWidget(audioGroup);
    auto* volumeLayout = new QHBoxLayout(volumeHost);
    volumeLayout->setContentsMargins(0, 0, 0, 0);
    auto* volumeSlider = new LiveSlider(Qt::Horizontal, volumeHost);
    auto* volumeLabel = new QLabel(volumeHost);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(qRound((_p->playbackCtrl ? _p->playbackCtrl->getVolume() : 1.0f) * 100.0f));
    volumeLabel->setText(QStringLiteral("%1%").arg(volumeSlider->value()));
    volumeLabel->setMinimumWidth(38);
    volumeLayout->addWidget(volumeSlider, 1);
    volumeLayout->addWidget(volumeLabel);
    auto* muteCheck = new QCheckBox(QStringLiteral("静音"), audioGroup);
    muteCheck->setChecked(_p->playbackCtrl && _p->playbackCtrl->isMuted());
    auto* offsetSpin = new QDoubleSpinBox(audioGroup);
    offsetSpin->setRange(-5.0, 5.0);
    offsetSpin->setDecimals(3);
    offsetSpin->setSingleStep(0.01);
    offsetSpin->setSuffix(QStringLiteral(" 秒"));
    offsetSpin->setValue(_p->playbackCtrl ? _p->playbackCtrl->getAudioOffset() : 0.0);
    auto* audioDevice = new QComboBox(audioGroup);
    if (_p->playbackCtrl) {
        audioDevice->addItems(_p->playbackCtrl->availableAudioDevices());
        const int deviceIndex = audioDevice->findText(_p->playbackCtrl->getAudioDeviceName());
        if (deviceIndex >= 0) audioDevice->setCurrentIndex(deviceIndex);
    }
    audioForm->addRow(QStringLiteral("音量"), volumeHost);
    audioForm->addRow(QString(), muteCheck);
    audioForm->addRow(QStringLiteral("音频同步"), offsetSpin);
    audioForm->addRow(QStringLiteral("输出设备"), audioDevice);
    auto* bitrateUnitCombo = new QComboBox(audioGroup);
    bitrateUnitCombo->setObjectName(QStringLiteral("SettingsBitrateUnit"));
    bitrateUnitCombo->setToolTip(QStringLiteral("选择播放器底部状态栏的码率单位；自动会根据数值选择 Mbps 或 kbps。"));
    bitrateUnitCombo->addItem(QStringLiteral("自动（推荐）"), QStringLiteral("auto"));
    bitrateUnitCombo->addItem(QStringLiteral("Mbps"), QStringLiteral("mbps"));
    bitrateUnitCombo->addItem(QStringLiteral("kbps"), QStringLiteral("kbps"));
    const QString savedBitrateUnit = _p->userSettings
        ? _p->userSettings->value(QStringLiteral("playback/bitrateUnit"), QStringLiteral("auto")).toString().trimmed().toLower()
        : QStringLiteral("auto");
    const int savedBitrateUnitIndex = bitrateUnitCombo->findData(savedBitrateUnit);
    bitrateUnitCombo->setCurrentIndex(savedBitrateUnitIndex >= 0 ? savedBitrateUnitIndex : 0);
    audioForm->addRow(QStringLiteral("码率单位"), bitrateUnitCombo);
    playbackLayout->addWidget(audioGroup);

    auto* readAheadCheck = new QCheckBox(QStringLiteral("启用智能预读"), playbackContent);
    readAheadCheck->setChecked(_p->playbackCtrl && _p->playbackCtrl->isReadAheadEnabled());
    playbackLayout->addWidget(readAheadCheck);
    playbackLayout->addStretch();
    connect(volumeSlider, &QSlider::valueChanged, this, [volumeLabel](int value) {
        volumeLabel->setText(QStringLiteral("%1%").arg(value));
    });
    connect(volumeSlider, &QSlider::sliderReleased, this, [this, volumeSlider] {
        if (_p->playbackCtrl) _p->playbackCtrl->setVolume(static_cast<float>(volumeSlider->value()) / 100.0f);
    });
    connect(muteCheck, &QCheckBox::toggled, this, [this](bool muted) {
        if (_p->playbackCtrl) _p->playbackCtrl->setMute(muted);
    });
    connect(offsetSpin, &QDoubleSpinBox::editingFinished, this, [this, offsetSpin] {
        if (_p->playbackCtrl) _p->playbackCtrl->setAudioOffset(offsetSpin->value());
    });
    connect(audioDevice, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (_p->playbackCtrl && !name.isEmpty()) _p->playbackCtrl->setAudioDevice(name);
    });
    connect(bitrateUnitCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, bitrateUnitCombo](int index) {
        const QString unit = bitrateUnitCombo->itemData(index).toString();
        if (_p->userSettings) {
            _p->userSettings->setValue(QStringLiteral("playback/bitrateUnit"), unit);
            _p->userSettings->sync();
        }
        if (_p->performanceService) _p->performanceService->setBitrateDisplayUnit(unit);
    });
    connect(readAheadCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (_p->playbackCtrl) _p->playbackCtrl->setReadAheadEnabled(enabled);
    });
    if (auto* playbackSignals = _p->playbackCtrl ? _p->playbackCtrl->signalProxy() : nullptr) {
        connect(playbackSignals, &PlaybackServiceSignals::volumeChanged, volumeSlider, [volumeSlider](float volume) {
            const QSignalBlocker blocker(volumeSlider);
            volumeSlider->setValue(qRound(volume * 100.0f));
        });
        connect(playbackSignals, &PlaybackServiceSignals::muteChanged, muteCheck, [muteCheck](bool muted) {
            const QSignalBlocker blocker(muteCheck);
            muteCheck->setChecked(muted);
        });
    }
    tabs->addTab(playbackTab, QStringLiteral("播放"));

    QWidget* performanceContent = nullptr;
    QVBoxLayout* performanceLayout = nullptr;
    auto* performanceTab = makeSettingsPage(&performanceContent, &performanceLayout);
    auto* decodeGroup = new QGroupBox(QStringLiteral("解码与缓存"), performanceContent);
    auto* decodeForm = new QFormLayout(decodeGroup);
    auto* hardwareCombo = new QComboBox(decodeGroup);
    auto* app = qobject_cast<Application*>(qApp);
    auto hw = app ? app->hwDecodeManager() : nullptr;
    for (int i = 0; i <= static_cast<int>(HwAccelType::Auto); ++i) {
        const auto type = static_cast<HwAccelType>(i);
        hardwareCombo->addItem(type == HwAccelType::Auto ? QStringLiteral("自动选择（推荐）") : hwAccelName(type), i);
    }
    if (hw) hardwareCombo->setCurrentIndex(static_cast<int>(hw->hwAccelType()));
    auto* ramCacheSpin = new QSpinBox(decodeGroup);
    ramCacheSpin->setRange(1, 64);
    ramCacheSpin->setSuffix(QStringLiteral(" GB"));
    ramCacheSpin->setValue(static_cast<int>(_p->cacheManager ? _p->cacheManager->ramCacheSizeGB() : 4));
    auto* aheadSpin = new QSpinBox(decodeGroup);
    aheadSpin->setRange(4, 512);
    aheadSpin->setValue(static_cast<int>(_p->cacheManager ? _p->cacheManager->readAheadFrames() : 32));
    auto* behindSpin = new QSpinBox(decodeGroup);
    behindSpin->setRange(0, 256);
    behindSpin->setValue(static_cast<int>(_p->cacheManager ? _p->cacheManager->readBehindFrames() : 8));
    decodeForm->addRow(QStringLiteral("硬件解码"), hardwareCombo);
    decodeForm->addRow(QStringLiteral("内存缓存"), ramCacheSpin);
    decodeForm->addRow(QStringLiteral("向前预读帧"), aheadSpin);
    decodeForm->addRow(QStringLiteral("向后保留帧"), behindSpin);
    performanceLayout->addWidget(decodeGroup);
    auto* clearCacheButton = new QPushButton(QStringLiteral("清空播放缓存"), performanceContent);
    performanceLayout->addWidget(clearCacheButton);
    performanceLayout->addStretch();
    connect(hardwareCombo, qOverload<int>(&QComboBox::activated), this, [hw, hardwareCombo](int index) {
        if (hw && index >= 0) hw->setHwAccelType(static_cast<HwAccelType>(hardwareCombo->itemData(index).toInt()));
    });
    connect(ramCacheSpin, &QSpinBox::editingFinished, this, [this, ramCacheSpin] {
        if (_p->cacheManager) _p->cacheManager->setRamCacheSizeGB(static_cast<size_t>(ramCacheSpin->value()));
    });
    connect(aheadSpin, &QSpinBox::editingFinished, this, [this, aheadSpin] {
        if (_p->cacheManager) _p->cacheManager->setReadAheadFrames(static_cast<size_t>(aheadSpin->value()));
    });
    connect(behindSpin, &QSpinBox::editingFinished, this, [this, behindSpin] {
        if (_p->cacheManager) _p->cacheManager->setReadBehindFrames(static_cast<size_t>(behindSpin->value()));
    });
    connect(clearCacheButton, &QPushButton::clicked, this, [this] {
        if (_p->cacheManager) _p->cacheManager->clear();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("播放缓存已清空"), 2500);
    });
    tabs->addTab(performanceTab, QStringLiteral("性能"));

    QWidget* subtitleContent = nullptr;
    QVBoxLayout* subtitleLayout = nullptr;
    auto* subtitleTab = makeSettingsPage(&subtitleContent, &subtitleLayout);
    auto* subtitleGroup = new QGroupBox(QStringLiteral("字幕识别与翻译"), subtitleContent);
    auto* subtitleForm = new QFormLayout(subtitleGroup);
    auto* translationVisible = new QCheckBox(QStringLiteral("显示字幕翻译"), subtitleGroup);
    translationVisible->setChecked(_p->playbackBar && _p->playbackBar->translationVisible());
    auto* modeCombo = new QComboBox(subtitleGroup);
    modeCombo->addItem(QStringLiteral("快速播放"), TranslationPlaybackStrategy::quickPlaybackModeId());
    modeCombo->addItem(QStringLiteral("高质量翻译"), TranslationPlaybackStrategy::highQualityModeId());
    const int modeIndex = modeCombo->findData(_p->translationPlaybackMode);
    modeCombo->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
    auto* languageCombo = new QComboBox(subtitleGroup);
    languageCombo->addItem(QStringLiteral("自动检测"), QStringLiteral("auto"));
    languageCombo->addItem(QStringLiteral("日语"), QStringLiteral("ja"));
    languageCombo->addItem(QStringLiteral("英语"), QStringLiteral("en"));
    languageCombo->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    const QString sourceLanguage = _p->userSettings
        ? _p->userSettings->value(QStringLiteral("ai/subtitles/sourceLanguage"), QStringLiteral("auto")).toString()
        : QStringLiteral("auto");
    const int languageIndex = languageCombo->findData(sourceLanguage);
    languageCombo->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);
    auto* refineCheck = new QCheckBox(QStringLiteral("后台精修字幕"), subtitleGroup);
    refineCheck->setChecked(_p->userSettings && _p->userSettings->value(QStringLiteral("ai/subtitles/refine/enabled"), false).toBool());
    subtitleForm->addRow(QString(), translationVisible);
    subtitleForm->addRow(QStringLiteral("翻译模式"), modeCombo);
    subtitleForm->addRow(QStringLiteral("源语言"), languageCombo);
    subtitleForm->addRow(QString(), refineCheck);
    subtitleLayout->addWidget(subtitleGroup);
    subtitleLayout->addStretch();
    connect(translationVisible, &QCheckBox::toggled, this, [this](bool visible) {
        if (_p->playbackBar) _p->playbackBar->setTranslationVisible(visible);
        if (_p->translationToggleAction) _p->translationToggleAction->setChecked(visible);
    });
    connect(modeCombo, &QComboBox::currentIndexChanged, this, [this, modeCombo](int index) {
        _setTranslationPlaybackMode(modeCombo->itemData(index).toString());
    });
    connect(languageCombo, &QComboBox::currentIndexChanged, this, [this, languageCombo](int index) {
        if (_p->userSettings) {
            _p->userSettings->setValue(QStringLiteral("ai/subtitles/sourceLanguage"), languageCombo->itemData(index));
        }
    });
    connect(refineCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (_p->userSettings) {
            _p->userSettings->setValue(QStringLiteral("ai/subtitles/refine/enabled"), enabled);
        }
    });
    tabs->addTab(subtitleTab, QStringLiteral("字幕"));

    auto* connectionScroll = new QScrollArea(tabs);
    connectionScroll->setWidgetResizable(true);
    connectionScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* connectionTab = new QWidget(connectionScroll);
    auto* connectionLayout = new QVBoxLayout(connectionTab);
    connectionLayout->setContentsMargins(0, 0, 0, 0);
    connectionLayout->setSpacing(0);
    auto* wizard = new AIConnectionWizardWidget(connectionTab);
    wizard->setProperty("cgplay.settingsSearchText",
        QStringLiteral("AI 服务连接 API Key Base URL Endpoint 模型 图片生成"));
    connectionLayout->addWidget(wizard);
    connectionScroll->setWidget(connectionTab);
    tabs->addTab(connectionScroll, QStringLiteral("AI"));
    tabs->setCurrentIndex(0);

    new SettingsNavigation(dialog, tabs, hostLayout);
    dialogLayout->addWidget(host, 1);
    auto* closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    if (auto* closeButton = closeButtons->button(QDialogButtonBox::Close)) {
        closeButton->setText(QStringLiteral("关闭"));
        closeButton->setAccessibleName(QStringLiteral("关闭设置"));
        closeButton->setAutoDefault(false);
    }
    connect(resetAllCustomizationButton, &QPushButton::clicked, this, [this, dialog, toolbarEditors] {
        const auto answer = QMessageBox::warning(
            dialog,
            QStringLiteral("恢复所有自定义设置"),
            QStringLiteral("将恢复默认外观、工作区、快捷键、工具栏以及鼠标和手柄绑定。此操作不可撤销，是否继续？"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;

        if (_p->userSettings) {
            const QStringList prefixes = {
                QStringLiteral("appearance/"), QStringLiteral("shortcuts/"),
                QStringLiteral("toolbar/"), QStringLiteral("workspace/"),
                QStringLiteral("input/")};
            for (const QString& key : _p->userSettings->allKeys()) {
                for (const QString& prefix : prefixes) {
                    if (key.startsWith(prefix)) {
                        _p->userSettings->remove(key);
                        break;
                    }
                }
            }
            _p->userSettings->sync();
        }

        for (auto& descriptor : _p->commandDescriptors) {
            descriptor.shortcut = _p->commandDefaultShortcuts.value(descriptor.id);
            const QKeySequence shortcut(_p->commandDefaultShortcuts.value(descriptor.id));
            for (QAction* action : findChildren<QAction*>(descriptor.id)) action->setShortcut(shortcut);
        }
        for (const auto& entry : *toolbarEditors) {
            entry.check->setChecked(true);
            const auto it = std::find_if(_p->toolbarContributions.cbegin(), _p->toolbarContributions.cend(),
                [&entry](const ToolbarContribution& value) { return value.id == entry.id; });
            if (it != _p->toolbarContributions.cend()) entry.order->setValue(it->order);
        }
        if (_p->annoToolbar) _p->annoToolbar->setVisible(true);
        _p->annoToolsVisible = true;
        _rebuildCustomToolbar();

        InputBindingStore paths;
        for (const auto device : {InputBindingStore::Device::Keyboard, InputBindingStore::Device::Mouse, InputBindingStore::Device::Gamepad}) {
            QFile::remove(paths.filePath(device));
        }
        _p->inputBindings = std::make_unique<InputBindingStore>();
        _p->inputBindings->load();

        const QString defaultWorkspace = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("workspaces/%1.json").arg(safeWorkspaceName(QStringLiteral("默认审片"))));
        QFile::remove(defaultWorkspace);
        QFile::remove(defaultWorkspace + QStringLiteral(".bak"));
        _p->leftVisible = true;
        _p->rightVisible = true;
        _p->activeSidePanel = 2;
        _applyAdaptiveSidePanelLayout(true);
        if (_p->windowSettings) {
            _p->windowSettings->setValue(QStringLiteral("layout/leftPanelVisible"), true);
            _p->windowSettings->setValue(QStringLiteral("layout/rightPanelVisible"), true);
            _p->windowSettings->remove(QStringLiteral("layout/sidePanelSizes"));
            _p->windowSettings->sync();
        }

        applyPlayerWindowOpacity(this, 100);
        qApp->setProperty("cgplay.deferAppearanceRefresh", false);
        qApp->setProperty("cgplay.appearanceRefreshPending", false);
        if (_p->appearanceRefreshTimer) _p->appearanceRefreshTimer->stop();
        dialog->hide();
        _p->settingsDialog = nullptr;
        dialog->deleteLater();
        QTimer::singleShot(0, qApp, [] {
            if (auto* app = qobject_cast<Application*>(qApp)) app->refreshAppearanceSettings();
        });
        if (statusBar()) statusBar()->showMessage(QStringLiteral("所有自定义设置已恢复默认"), 3000);
    });
    const auto finalizeSettings = [this] {
        if (_p->userSettings) _p->userSettings->sync();
        if (_p->appearanceRefreshTimer) _p->appearanceRefreshTimer->stop();
        qApp->setProperty("cgplay.deferAppearanceRefresh", false);
        const bool refreshPending = qApp->property("cgplay.appearanceRefreshPending").toBool();
        qApp->setProperty("cgplay.appearanceRefreshPending", false);
        if (refreshPending) {
            QTimer::singleShot(0, qApp, [] {
                if (auto* app = qobject_cast<Application*>(qApp)) app->refreshAppearanceSettings();
            });
        }
    };
    connect(closeButtons, &QDialogButtonBox::rejected, dialog, &QDialog::hide);
    connect(closeButtons, &QDialogButtonBox::rejected, this, finalizeSettings);
    connect(dialog, &QDialog::rejected, this, finalizeSettings);
    dialogLayout->addWidget(closeButtons);
    _p->settingsDialog = dialog;
    // Opening an editor does not change the theme. Reuse its prepared style
    // instead of repolishing the player, viewer and Codex workspace globally.
    const QString dialogStyle = qApp->property("cgplay.settingsDialogStyleSheet").toString();
    if (!dialogStyle.isEmpty()) dialog->setStyleSheet(dialogStyle);
}

} // namespace cgplay
