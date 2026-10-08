#include "ApplicationInternal.h"
#include "services/platform/WindowsFileAssociations.h"
#include <QListWidget>
#include <QStandardItemModel>
#include <QScopeGuard>
#include <QTabBar>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
namespace {

void settleSettings(int milliseconds = 40)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

class SettingsStyleObserver final : public QObject
{
public:
    int changes = 0;
protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::StyleChange) ++changes;
        return false;
    }
};

void settingsMouse(QWidget* target, QEvent::Type type, const QPoint& position)
{
    const bool released = type == QEvent::MouseButtonRelease;
    QMouseEvent event(type, QPointF(position), QPointF(target->mapToGlobal(position)),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        released ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

bool settingsClickItem(QListWidget* list, int row)
{
    if (!list || row < 0 || row >= list->count()) return false;
    auto* item = list->item(row);
    list->scrollToItem(item);
    const QRect rect = list->visualItemRect(item);
    if (rect.isEmpty()) return false;
    settingsMouse(list->viewport(), QEvent::MouseButtonPress, rect.center());
    settingsMouse(list->viewport(), QEvent::MouseButtonRelease, rect.center());
    return true;
}

void settingsKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(target, &release);
}

} // namespace

QJsonArray MainWindow::_runSettingsSmokeChecks()
{
    QJsonArray results;
    const bool background = qApp->property("cgplay.automationBackground").toBool();
    const bool isolated = _p->userSettings &&
        _p->userSettings->application().startsWith(QStringLiteral("CGPlayAutomation_"));
    const auto add = [&](const QString& name, bool passed, QJsonObject details = {}) {
        int visibleWindows = 0;
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->isVisible() && !widget->testAttribute(Qt::WA_DontShowOnScreen)) ++visibleWindows;
        int nativeVisibleWindows = 0;
#ifdef Q_OS_WIN
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == GetCurrentProcessId() && IsWindowVisible(window))
                ++*reinterpret_cast<int*>(data);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&nativeVisibleWindows));
#endif
        details.insert("background", background);
        details.insert("isolatedSettings", isolated);
        details.insert("visiblePlatformWindows", visibleWindows);
        details.insert("nativeVisibleWindows", nativeVisibleWindows);
        qInfo().noquote() << "[SettingsSmoke]" << name << (passed ? "PASS" : "FAIL");
        results.append(QJsonObject{{"name", name},
            {"status", passed && background && isolated && visibleWindows == 0 && nativeVisibleWindows == 0 ? "PASS" : "FAIL"},
            {"message", "settings interaction regression"}, {"details", details}});
    };
    if (!background || !isolated || !_p->reviewPanel) {
        add(QStringLiteral("Settings automation isolation"), false);
        return results;
    }

    const QStringList args = QCoreApplication::arguments();
    const int outputArg = args.indexOf(QStringLiteral("--smoke-output"));
    const QDir outputDir(outputArg >= 0 && outputArg + 1 < args.size()
        ? QFileInfo(args[outputArg + 1]).absolutePath()
        : QDir::current().absoluteFilePath(QStringLiteral("tests/artifacts/settings_smoke")));
    QDir().mkpath(outputDir.absolutePath());
    const QString automaticUpdateKey = QStringLiteral("updates/checkAutomatically");
    const bool hadAutomaticUpdateSetting = _p->userSettings->contains(automaticUpdateKey);
    const QVariant originalAutomaticUpdateSetting = _p->userSettings->value(automaticUpdateKey);
    const auto restoreAutomaticUpdateSetting = [&] {
        if (hadAutomaticUpdateSetting)
            _p->userSettings->setValue(automaticUpdateKey, originalAutomaticUpdateSetting);
        else
            _p->userSettings->remove(automaticUpdateKey);
        _p->userSettings->sync();
    };
    const auto automaticUpdateCleanup = qScopeGuard(restoreAutomaticUpdateSetting);
    _p->userSettings->remove(automaticUpdateKey);
    _p->userSettings->sync();
    SettingsStyleObserver observer;
    installEventFilter(&observer);
    const bool wasConstructed = !_p->settingsDialog.isNull();
    const QString playerStyle = qApp->styleSheet();
    QElapsedTimer timer;
    timer.start();
    _installSettingsWidgets(_p->reviewPanel);
    const double constructionMs = timer.nsecsElapsed() / 1e6;
    auto* dialog = _p->settingsDialog.data();
    if (!dialog) {
        removeEventFilter(&observer);
        add(QStringLiteral("Settings construction"), false);
        return results;
    }
    dialog->setAttribute(Qt::WA_DontShowOnScreen);
    dialog->setAttribute(Qt::WA_ShowWithoutActivating);
    qApp->setProperty("cgplay.deferAppearanceRefresh", true);
    dialog->show();
    timer.restart();
    settleSettings();
    const double firstSettleMs = timer.nsecsElapsed() / 1e6;
    auto* host = dialog->findChild<QWidget*>(QStringLiteral("AISettingsWorkspaceWidget"));
    auto* tabs = host ? host->findChild<QTabWidget*>(QStringLiteral("SettingsPages")) : nullptr;
    const QString cachedStyle = qApp->property("cgplay.settingsDialogStyleSheet").toString();
    add(QStringLiteral("Settings lazy construction and local theme"), !wasConstructed && tabs &&
        observer.changes == 0 && qApp->styleSheet() == playerStyle &&
        !cachedStyle.isEmpty() && dialog->styleSheet() == cachedStyle,
        {{"firstConstructionMs", constructionMs}, {"firstSettleMs", firstSettleMs},
         {"settleMinimumMs", 40}, {"playerStyleChanges", observer.changes}, {"wasConstructedAtStartup", wasConstructed}});
    removeEventFilter(&observer);
    if (!tabs) {
        dialog->reject();
        return results;
    }

    auto* automaticUpdates = dialog->findChild<QCheckBox*>(QStringLiteral("AutomaticUpdateCheck"));
    const bool automaticDefault = automaticUpdates && automaticUpdates->isChecked();
    const auto previousCheckState = _p->updateCheckState;
    const bool previousCheckInteractive = _p->updateCheckInteractive;
    const bool previousCheckInProgress = _p->updateCheckInProgress;
    const auto cancelledBackground = std::make_shared<UpdateTransferState>();
    const auto explicitCheck = std::make_shared<UpdateTransferState>();
    bool savedAutomaticDisabled = false;
    bool savedAutomaticEnabled = false;
    bool interactiveCheckPreserved = false;
    if (automaticUpdates) {
        // Test cancellation with data-only tokens; never start a network worker.
        _p->updateCheckState = cancelledBackground;
        _p->updateCheckInteractive = false;
        automaticUpdates->click();
        savedAutomaticDisabled = !automaticUpdates->isChecked() &&
            _p->userSettings->contains(automaticUpdateKey) &&
            !_p->userSettings->value(automaticUpdateKey).toBool() && cancelledBackground->cancelled.load();
        automaticUpdates->click();
        savedAutomaticEnabled = automaticUpdates->isChecked() &&
            _p->userSettings->value(automaticUpdateKey).toBool();
        _p->updateCheckState = explicitCheck;
        _p->updateCheckInteractive = true;
        automaticUpdates->click();
        interactiveCheckPreserved = !automaticUpdates->isChecked() && !explicitCheck->cancelled.load();
    }
    const bool noCheckDispatched = _p->updateCheckState == (automaticUpdates ? explicitCheck : previousCheckState) &&
        _p->updateCheckInProgress == previousCheckInProgress;
    _p->updateCheckState = previousCheckState;
    _p->updateCheckInteractive = previousCheckInteractive;
    restoreAutomaticUpdateSetting();
    if (automaticUpdates) {
        const QSignalBlocker blocker(automaticUpdates);
        automaticUpdates->setChecked(!hadAutomaticUpdateSetting || originalAutomaticUpdateSetting.toBool());
    }
    const bool automaticSettingRestored = _p->userSettings->contains(automaticUpdateKey) == hadAutomaticUpdateSetting &&
        (!hadAutomaticUpdateSetting || _p->userSettings->value(automaticUpdateKey) == originalAutomaticUpdateSetting);
    add(QStringLiteral("Settings automatic update preference and cancellation"),
        automaticDefault && savedAutomaticDisabled && savedAutomaticEnabled && interactiveCheckPreserved &&
        noCheckDispatched && automaticSettingRestored,
        {{"defaultEnabled", automaticDefault}, {"disabledPersistedAndBackgroundCancelled", savedAutomaticDisabled},
         {"enabledPersisted", savedAutomaticEnabled}, {"explicitCheckPreserved", interactiveCheckPreserved},
         {"noNetworkCheckDispatched", noCheckDispatched}, {"isolatedSettingRestored", automaticSettingRestored}});

    const QStringList expectedTabs{QStringLiteral("常规"), QStringLiteral("外观与布局"), QStringLiteral("工作区布局"),
        QStringLiteral("快捷键"), QStringLiteral("工具栏"), QStringLiteral("鼠标与手柄"), QStringLiteral("命令管理"),
        QStringLiteral("播放"), QStringLiteral("性能"), QStringLiteral("字幕"), QStringLiteral("AI")};
    auto* navigation = dialog->findChild<QListWidget*>(QStringLiteral("SettingsCategoryNavigation"));
    auto* pageTitle = dialog->findChild<QLabel*>(QStringLiteral("SettingsPageTitle"));
    auto* pageDescription = dialog->findChild<QLabel*>(QStringLiteral("SettingsPageDescription"));
    auto* search = dialog->findChild<QLineEdit*>(QStringLiteral("SettingsSearchEdit"));
    auto* searchResults = dialog->findChild<QListWidget*>(QStringLiteral("SettingsSearchResults"));
    auto* emptyState = dialog->findChild<QLabel*>(QStringLiteral("SettingsSearchEmptyState"));
    const bool navigationPresent = navigation && pageTitle && pageDescription && search && searchResults && emptyState;
    add(QStringLiteral("Settings navigation controls present"), navigationPresent &&
        tabs->tabBar()->isHidden() && navigation->count() == expectedTabs.size());
    if (!navigationPresent) {
        dialog->reject();
        return results;
    }
    const auto associationChecks = dialog->findChildren<QCheckBox*>(QRegularExpression(QStringLiteral("^SettingsFileAssociation_")));
    auto* openDefaultApps = dialog->findChild<QPushButton*>(QStringLiteral("SettingsFileAssociationOpenDefaultApps"));
    auto* deployDeviceDefaults = dialog->findChild<QPushButton*>(QStringLiteral("SettingsFileAssociationDeployDeviceDefaults"));
    auto* removeDeviceDefaults = dialog->findChild<QPushButton*>(QStringLiteral("SettingsFileAssociationRemoveDeviceDefaults"));
    auto* associationStatus = dialog->findChild<QLabel*>(QStringLiteral("SettingsFileAssociationStatus"));
    const bool associationControls = associationChecks.size() == WindowsFileAssociations::supportedVideoExtensions().size() &&
        dialog->findChild<QPushButton*>(QStringLiteral("SettingsFileAssociationSelectAll")) &&
        dialog->findChild<QPushButton*>(QStringLiteral("SettingsFileAssociationClearAll")) &&
        dialog->findChild<QPushButton*>(QStringLiteral("SettingsFileAssociationApply")) &&
        openDefaultApps && openDefaultApps->text() == QStringLiteral("打开 CGPlay 默认应用页") &&
        deployDeviceDefaults && deployDeviceDefaults->text() == QStringLiteral("管理员部署全部默认") &&
        removeDeviceDefaults && removeDeviceDefaults->text() == QStringLiteral("撤销设备级策略") &&
        associationStatus && associationStatus->text().contains(QStringLiteral("当前系统实际使用 CGPlay"));
    add(QStringLiteral("Settings file association controls"), associationControls,
        {{"supportedVideoExtensions", WindowsFileAssociations::supportedVideoExtensions().size()},
         {"checkboxCount", associationChecks.size()},
         {"effectiveStatusPresent", associationStatus && associationStatus->text().contains(QStringLiteral("当前系统实际使用 CGPlay"))},
         {"registryWritesInvoked", false},
         {"background", background}});
    auto* bitrateUnitCombo = dialog->findChild<QComboBox*>(QStringLiteral("SettingsBitrateUnit"));
    const QString bitrateUnitKey = QStringLiteral("playback/bitrateUnit");
    const bool hadBitrateUnit = _p->userSettings->contains(bitrateUnitKey);
    const QVariant originalBitrateUnit = _p->userSettings->value(bitrateUnitKey);
    const QString originalServiceBitrateUnit = _p->performanceService
        ? _p->performanceService->bitrateDisplayUnit()
        : QStringLiteral("auto");
    QJsonArray bitrateUnitChecks;
    bool bitrateUnitPassed = bitrateUnitCombo &&
        bitrateUnitCombo->findData(QStringLiteral("auto")) >= 0 &&
        bitrateUnitCombo->findData(QStringLiteral("mbps")) >= 0 &&
        bitrateUnitCombo->findData(QStringLiteral("kbps")) >= 0;
    for (const QString& unit : {QStringLiteral("auto"), QStringLiteral("mbps"), QStringLiteral("kbps")}) {
        const int index = bitrateUnitCombo ? bitrateUnitCombo->findData(unit) : -1;
        if (bitrateUnitCombo && index >= 0) {
            bitrateUnitCombo->setCurrentIndex(index);
            settleSettings();
        }
        const QString label = _p->lblBitrate ? _p->lblBitrate->text() : QString();
        const bool labelMatches = unit == QStringLiteral("auto")
            ? (label.contains(QStringLiteral("Mbps")) || label.contains(QStringLiteral("kbps")) || label.endsWith(QStringLiteral("--")))
            : label.endsWith(unit == QStringLiteral("mbps") ? QStringLiteral("Mbps") : QStringLiteral("kbps"));
        const bool persisted = _p->userSettings->value(bitrateUnitKey).toString() == unit ||
            (unit == QStringLiteral("auto") && !hadBitrateUnit && !_p->userSettings->contains(bitrateUnitKey));
        bitrateUnitPassed = bitrateUnitPassed && index >= 0 && labelMatches && persisted &&
            (!_p->performanceService || _p->performanceService->bitrateDisplayUnit() == unit);
        bitrateUnitChecks.append(QJsonObject{{"unit", unit}, {"index", index}, {"label", label},
            {"labelMatches", labelMatches}, {"persisted", persisted}});
    }
    if (hadBitrateUnit) _p->userSettings->setValue(bitrateUnitKey, originalBitrateUnit);
    else _p->userSettings->remove(bitrateUnitKey);
    _p->userSettings->sync();
    if (_p->performanceService) _p->performanceService->setBitrateDisplayUnit(originalServiceBitrateUnit);
    if (bitrateUnitCombo) {
        const int restoreIndex = bitrateUnitCombo->findData(originalServiceBitrateUnit);
        if (restoreIndex >= 0) {
            const QSignalBlocker blocker(bitrateUnitCombo);
            bitrateUnitCombo->setCurrentIndex(restoreIndex);
        }
    }
    add(QStringLiteral("Settings bitrate unit display and persistence"), bitrateUnitPassed,
        {{"controlPresent", bitrateUnitCombo != nullptr}, {"checks", bitrateUnitChecks},
         {"restoredOriginalSetting", _p->userSettings->contains(bitrateUnitKey) == hadBitrateUnit &&
             (!hadBitrateUnit || _p->userSettings->value(bitrateUnitKey) == originalBitrateUnit)},
         {"background", background}});
    if (qEnvironmentVariableIsSet("CGPLAY_TEST_FILE_ASSOCIATIONS")) {
        const QSet<QString> before = WindowsFileAssociations::selectedVideoExtensions();
        QSet<QString> probeSelection;
        if (!WindowsFileAssociations::supportedVideoExtensions().isEmpty()) {
            probeSelection.insert(WindowsFileAssociations::supportedVideoExtensions().constFirst());
        }
        QString applyError;
        const bool applied = WindowsFileAssociations::applyVideoExtensions(probeSelection, &applyError);
        const bool observed = applied && WindowsFileAssociations::selectedVideoExtensions() == probeSelection;
        QString restoreError;
        const bool restored = WindowsFileAssociations::applyVideoExtensions(before, &restoreError);
        add(QStringLiteral("Settings file association registry roundtrip"), applied && observed && restored,
            {{"applied", applied}, {"observed", observed}, {"restored", restored},
             {"applyError", applyError}, {"restoreError", restoreError}, {"selectedBefore", before.size()},
             {"selectedProbe", probeSelection.size()}, {"registryWritesInvoked", true}});
    }
    bool pagesPassed = tabs->count() == expectedTabs.size();
    QJsonArray pages;
    for (int index = 0; index < tabs->count(); ++index) {
        timer.restart();
        const bool clicked = settingsClickItem(navigation, index);
        const double dispatchMs = timer.nsecsElapsed() / 1e6;
        settleSettings();
        auto* page = tabs->widget(index);
        const auto children = page->findChildren<QWidget*>();
        int controls = 0;
        QJsonArray buttons;
        for (auto* child : children) {
            if (qobject_cast<QAbstractButton*>(child) || qobject_cast<QAbstractSpinBox*>(child) ||
                qobject_cast<QComboBox*>(child) || qobject_cast<QLineEdit*>(child) ||
                qobject_cast<QSlider*>(child) || qobject_cast<QTableWidget*>(child)) ++controls;
            if (auto* button = qobject_cast<QAbstractButton*>(child)) {
                // Inventory button labels and identifiers only; editable values
                // and provider diagnostics are deliberately excluded.
                if (!button->text().trimmed().isEmpty())
                    buttons.append(QJsonObject{{"text", button->text()}, {"objectName", button->objectName()},
                        {"type", QString::fromLatin1(button->metaObject()->className())}, {"enabled", button->isEnabled()}});
            }
        }
        const QString capture = outputDir.filePath(QStringLiteral("settings_tab_%1.png").arg(index, 2, 10, QChar('0')));
        // PrintWindow cannot read WA_DontShowOnScreen surfaces. QWidget::grab
        // renders this exact dialog without showing or activating a native UI.
        const bool saved = dialog->grab().save(capture);
        pagesPassed = pagesPassed && clicked && tabs->currentIndex() == index &&
            index < expectedTabs.size() && tabs->tabText(index) == expectedTabs[index] &&
            navigation->item(index)->text().contains(expectedTabs[index]) && pageTitle->text() == expectedTabs[index] &&
            !pageDescription->text().trimmed().isEmpty() && page->isVisible() && controls > 0 && saved;
        pages.append(QJsonObject{{"title", tabs->tabText(index)}, {"controls", controls},
            {"buttons", buttons}, {"switchDispatchMs", dispatchMs}, {"capture", capture}});
    }
    add(QStringLiteral("Settings all pages and captures"), pagesPassed,
        {{"pageCount", tabs->count()}, {"pages", pages}, {"captureMethod", "QWidget::grab (background surface)"}});

    // Preserve every pre-redesign action without activating network requests,
    // file pickers or destructive reset operations merely to count a button.
    const QList<QStringList> expectedButtons{
        {QStringLiteral("显示播放列表"), QStringLiteral("显示右侧审片面板"), QStringLiteral("显示批注工具"),
         QStringLiteral("显示 AI 工作台"), QStringLiteral("LUT / OCIO 色彩设置"), QStringLiteral("恢复默认面板布局"),
         QStringLiteral("检查版本更新"), QStringLiteral("检查缺失组件"), QStringLiteral("恢复所有自定义设置"),
         QStringLiteral("全选"), QStringLiteral("取消全选"), QStringLiteral("应用关联"), QStringLiteral("打开 CGPlay 默认应用页"),
         QStringLiteral("管理员部署全部默认"), QStringLiteral("撤销设备级策略")},
        {QStringLiteral("选择背景颜色"), QStringLiteral("选择背景结束色"), QStringLiteral("选择面板颜色"),
         QStringLiteral("选择工具栏颜色"), QStringLiteral("选择时间轴颜色"), QStringLiteral("选择字体颜色"),
         QStringLiteral("选择边框颜色"), QStringLiteral("选择强调色"), QStringLiteral("浏览"), QStringLiteral("保存"),
         QStringLiteral("加载"), QStringLiteral("删除"), QStringLiteral("恢复主题默认"), QStringLiteral("播放时动态背景")},
        {QStringLiteral("保存当前布局为预设"), QStringLiteral("导出配置"), QStringLiteral("导入配置")},
        {QStringLiteral("保存快捷键"), QStringLiteral("恢复默认快捷键")},
        {QStringLiteral("保存工具栏配置"), QStringLiteral("保存/更新"), QStringLiteral("清空全部")},
        {QStringLiteral("保存鼠标与手柄绑定")}, {},
        {QStringLiteral("静音"), QStringLiteral("启用智能预读")}, {QStringLiteral("清空播放缓存")},
        {QStringLiteral("显示字幕翻译"), QStringLiteral("后台精修字幕")},
        {QStringLiteral("自动选择推荐模型"), QStringLiteral("自动识别"), QStringLiteral("保存"), QStringLiteral("保存图片生成服务")}
    };
    bool inventoryPassed = tabs->count() == expectedButtons.size();
    QJsonArray missingButtons;
    int auditedButtons = 0;
    for (int page = 0; page < qMin(tabs->count(), int(expectedButtons.size())); ++page) {
        for (const auto& label : expectedButtons[page]) {
            bool found = false;
            for (auto* button : tabs->widget(page)->findChildren<QAbstractButton*>())
                found = found || button->text().startsWith(label);
            // Shortcut actions are intentionally sticky outside the scroll page.
            if (page == 3 && !found)
                for (auto* button : dialog->findChildren<QPushButton*>()) found = found || button->text() == label;
            if (found) ++auditedButtons;
            else { inventoryPassed = false; missingButtons.append(QStringLiteral("%1: %2").arg(page).arg(label)); }
        }
    }
    add(QStringLiteral("Settings existing action inventory preserved"), inventoryPassed,
        {{"auditedActions", auditedButtons}, {"missing", missingButtons},
         {"scope", "Presence audit; external/destructive actions are not invoked by this UI regression"}});

    settingsClickItem(navigation, 0);
    settingsKey(navigation, Qt::Key_Down);
    const bool keyDownWorked = navigation->currentRow() == 1 && tabs->currentIndex() == 1;
    settingsKey(navigation, Qt::Key_Up);
    add(QStringLiteral("Settings category keyboard navigation"), keyDownWorked && navigation->currentRow() == 0 &&
        tabs->currentIndex() == 0 && pageTitle->text() == expectedTabs[0]);

    const auto searchFor = [&](const QString& query) {
        search->setText(query);
        // A bounded settle also supports a short UI debounce without timing races.
        settleSettings(180);
    };
    const auto resultForPage = [&](int page) {
        for (int row = 0; row < searchResults->count(); ++row)
            if (searchResults->item(row)->data(Qt::UserRole).toInt() == page) return row;
        return -1;
    };
    bool searchPassed = true;
    QJsonArray queries;
    const QList<QPair<QString, int>> searchCases{
        {QStringLiteral("内存缓存"), 8}, {QStringLiteral("默认播放器"), 0}, {QStringLiteral("  PLAYBACK.NEXTFRAME  "), 3},
        {QStringLiteral("播放 暂停"), 3}, {QStringLiteral("字幕翻译"), 9}, {QStringLiteral("API Key"), 10}
    };
    for (const auto& test : searchCases) {
        timer.restart();
        searchFor(test.first);
        const double elapsedMs = timer.nsecsElapsed() / 1e6;
        const int row = resultForPage(test.second);
        const bool matches = row >= 0 && searchResults->isVisible() && !emptyState->isVisible();
        QString targetName;
        if (row >= 0) targetName = searchResults->item(row)->data(Qt::UserRole + 2).toString();
        const bool clicked = matches && settingsClickItem(searchResults, row);
        settleSettings();
        auto* target = targetName.isEmpty() ? nullptr : dialog->findChild<QWidget*>(targetName);
        bool targetInViewport = false;
        if (target) for (auto* parent = target->parentWidget(); parent; parent = parent->parentWidget()) {
            if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
                const QRect targetRect(target->mapTo(scroll->viewport(), QPoint()), target->size());
                targetInViewport = scroll->viewport()->rect().intersects(targetRect);
                break;
            }
        }
        const bool targetFocused = target && (target->focusPolicy() == Qt::NoFocus || dialog->focusWidget() == target ||
            (dialog->focusWidget() && target->isAncestorOf(dialog->focusWidget())));
        const bool landed = clicked && tabs->currentIndex() == test.second && navigation->currentRow() == test.second &&
            pageTitle->text() == expectedTabs[test.second] && target && target->isVisible() && targetInViewport && targetFocused;
        searchPassed = searchPassed && landed;
        queries.append(QJsonObject{{"query", test.first}, {"expectedPage", test.second}, {"matched", matches},
            {"landed", landed}, {"target", targetName}, {"targetInViewport", targetInViewport},
            {"targetFocusCorrect", targetFocused}, {"elapsedIncludingSettleMs", elapsedMs}});
    }
    searchFor(QStringLiteral("内存缓存"));
    const int cacheResult = resultForPage(8);
    if (cacheResult >= 0) searchResults->setCurrentRow(cacheResult);
    settingsKey(searchResults, Qt::Key_Return);
    settleSettings();
    const bool enterLanded = cacheResult >= 0 && tabs->currentIndex() == 8;
    searchFor(QStringLiteral("快捷键"));
    const QString searchCapture = outputDir.filePath(QStringLiteral("settings_search_results.png"));
    const bool searchSaved = dialog->grab().save(searchCapture);
    add(QStringLiteral("Settings search matching and target navigation"), searchPassed && enterLanded && searchSaved,
        {{"queries", queries}, {"resultEnterNavigation", enterLanded}, {"capture", searchCapture}});

    // Exercise mutable data exclusion using a synthetic value in isolated UI
    // fields only. Never log or persist original field contents or credentials.
    const QString privateProbe = QStringLiteral("CGPlayPrivateSearchProbe_793A");
    QHash<QLineEdit*, QString> privateEdits;
    for (auto* edit : tabs->widget(10)->findChildren<QLineEdit*>()) {
        privateEdits.insert(edit, edit->text());
        const QSignalBlocker blocker(edit);
        edit->setText(privateProbe);
    }
    auto* dynamicLabel = new QLabel(privateProbe, tabs->widget(10));
    dynamicLabel->hide();
    searchFor(privateProbe);
    const bool privateExcluded = !privateEdits.isEmpty() && searchResults->count() == 0 && emptyState->isVisible();
    delete dynamicLabel;
    for (auto it = privateEdits.cbegin(); it != privateEdits.cend(); ++it) {
        const QSignalBlocker blocker(it.key());
        it.key()->setText(it.value());
    }
    searchFor(QStringLiteral("no_such_setting_97512"));
    const bool noResult = searchResults->count() == 0 && emptyState->isVisible();
    const QString emptyCapture = outputDir.filePath(QStringLiteral("settings_search_empty.png"));
    const bool emptySaved = dialog->grab().save(emptyCapture);
    search->selectAll();
    settingsKey(search, Qt::Key_Backspace);
    settleSettings(180);
    const bool cleared = search->text().isEmpty() && !searchResults->isVisible() && !emptyState->isVisible() &&
        navigation->count() == expectedTabs.size() && tabs->isVisible();
    // Warm one result list before checking repeated query allocation stability.
    searchFor(QStringLiteral("内存缓存"));
    searchFor(QString());
    const auto searchWidgetCount = dialog->findChildren<QWidget*>().size();
    timer.restart();
    for (int iteration = 0; iteration < 40; ++iteration)
        search->setText(iteration % 2 ? QStringLiteral("PLAYBACK") : QStringLiteral("内存缓存"));
    settleSettings(180);
    searchFor(QString());
    const double repeatedSearchMs = timer.nsecsElapsed() / 1e6;
    add(QStringLiteral("Settings search privacy clearing and stable reuse"), privateExcluded && noResult && emptySaved && cleared &&
        dialog->findChildren<QWidget*>().size() == searchWidgetCount,
        {{"privateValuesExcluded", privateExcluded}, {"noResultState", noResult}, {"clearRestoresPages", cleared},
         {"queryChanges", 40}, {"elapsedIncludingSettleMs", repeatedSearchMs}, {"capture", emptyCapture}});

    int unexpectedSearchClicks = 0;
    QList<QMetaObject::Connection> searchClickObservers;
    for (auto* button : dialog->findChildren<QPushButton*>())
        searchClickObservers.append(connect(button, &QPushButton::clicked, this, [&unexpectedSearchClicks] { ++unexpectedSearchClicks; }));
    settingsClickItem(navigation, 0);
    settingsKey(navigation, Qt::Key_F, Qt::ControlModifier);
    const bool findFocusedSearch = dialog->focusWidget() == search;
    searchFor(QStringLiteral("内存缓存"));
    settingsKey(search, Qt::Key_Down);
    const bool downFocusedResults = dialog->focusWidget() == searchResults && searchResults->currentRow() == 0;
    settingsKey(searchResults, Qt::Key_Escape);
    const bool escapeCleared = search->text().isEmpty() && dialog->isVisible() && !searchResults->isVisible();
    settingsKey(search, Qt::Key_Return);
    const bool emptyEnterSafe = dialog->isVisible() && searchResults->count() == 0;
    searchFor(QStringLiteral("内存缓存"));
    settingsKey(search, Qt::Key_Return);
    settleSettings();
    const bool nonemptyEnterSafe = dialog->isVisible() && tabs->currentIndex() == 8 && search->text().isEmpty();
    for (const auto& connection : searchClickObservers) QObject::disconnect(connection);
    add(QStringLiteral("Settings search keyboard and empty Enter safety"),
        findFocusedSearch && downFocusedResults && escapeCleared && emptyEnterSafe && nonemptyEnterSafe && unexpectedSearchClicks == 0,
        {{"findFocus", findFocusedSearch}, {"downFocus", downFocusedResults},
         {"escapeClearsWithoutClose", escapeCleared}, {"emptyEnterDoesNotActivateAction", emptyEnterSafe},
         {"nonemptyEnterOnlyNavigates", nonemptyEnterSafe}, {"unexpectedPushButtonClicks", unexpectedSearchClicks}});

    QHash<QString, QString> effectiveBindings;
    for (const auto& descriptor : _p->commandDescriptors) effectiveBindings.insert(descriptor.id, descriptor.shortcut);
    QSet<QString> presentedCommands;
    QJsonArray presentationFailures;
    int presentedButtons = 0;
    int presentedActions = 0;
    const auto expectedHintKey = [&](const QString& id) {
        const QString key = QKeySequence(effectiveBindings.value(id)).toString(QKeySequence::NativeText);
        return key.isEmpty() ? QStringLiteral("快捷键：未设置") : QStringLiteral("快捷键：") + key;
    };
    for (auto* widget : QApplication::allWidgets()) {
        auto* button = qobject_cast<QAbstractButton*>(widget);
        if (!button || !button->property("cgplay.commandPresentation.ready").toBool()) continue;
        const QString id = button->property("commandId").toString();
        ++presentedButtons;
        presentedCommands.insert(id);
        if (!effectiveBindings.contains(id) || !button->toolTip().contains(expectedHintKey(id)) ||
            button->accessibleName().trimmed().isEmpty() || button->accessibleDescription() != button->toolTip() ||
            button->statusTip() != button->toolTip() ||
            QKeySequence(button->property("cgplay.commandPresentation.shortcut").toString()) != QKeySequence(effectiveBindings.value(id)))
            presentationFailures.append(QJsonObject{{"type", "button"}, {"command", id}, {"objectName", button->objectName()}});
    }
    for (auto* action : findChildren<QAction*>()) {
        if (!action->property("cgplay.commandPresentation.ready").toBool()) continue;
        QString id = action->property("commandId").toString();
        if (id.isEmpty()) id = action->property("cgplay.command.id").toString();
        if (id.isEmpty()) id = action->property("cgplay.commandPresentation.id").toString();
        ++presentedActions;
        presentedCommands.insert(id);
        if (!effectiveBindings.contains(id) || !action->toolTip().contains(expectedHintKey(id)) ||
            action->statusTip() != action->toolTip() ||
            QKeySequence(action->property("cgplay.commandPresentation.shortcut").toString()) != QKeySequence(effectiveBindings.value(id)))
            presentationFailures.append(QJsonObject{{"type", "action"}, {"command", id}, {"objectName", action->objectName()}});
    }
    QStringList presentedIds = presentedCommands.values();
    presentedIds.sort();
    add(QStringLiteral("Settings all mapped player command hints consistent"),
        presentedButtons > 5 && presentedActions > 10 && presentationFailures.isEmpty(),
        {{"buttons", presentedButtons}, {"actions", presentedActions}, {"commandIds", QJsonArray::fromStringList(presentedIds)},
         {"failures", presentationFailures}});

    int expectedCommands = 0;
    for (const auto& descriptor : _p->commandDescriptors)
        if (!descriptor.id.trimmed().isEmpty()) ++expectedCommands;
    auto* inputModel = dialog->findChild<QStandardItemModel*>(QStringLiteral("SettingsInputCommandModel"));
    QList<QComboBox*> bindingEditors;
    for (auto* combo : dialog->findChildren<QComboBox*>())
        if (combo->objectName().startsWith(QStringLiteral("SettingsBinding_"))) bindingEditors.append(combo);
    bool bindingsPassed = inputModel && inputModel->rowCount() == expectedCommands + 1 && bindingEditors.size() == 16;
    for (auto* combo : bindingEditors)
        bindingsPassed = bindingsPassed && combo->model() == inputModel && combo->count() == expectedCommands + 1;
    if (bindingEditors.size() >= 2 && inputModel && inputModel->rowCount() > 1) {
        const int originalIndex = bindingEditors[0]->currentIndex();
        const int otherIndex = bindingEditors[1]->currentIndex();
        bindingEditors[0]->setCurrentIndex(originalIndex == 0 ? 1 : 0);
        bindingsPassed = bindingsPassed && bindingEditors[1]->currentIndex() == otherIndex;
        bindingEditors[0]->setCurrentIndex(originalIndex);
    }
    auto* table = tabs->widget(6)->findChild<QTableWidget*>();
    add(QStringLiteral("Settings shared command choices"), bindingsPassed &&
        dialog->findChildren<QKeySequenceEdit*>().size() == expectedCommands && table &&
        table->rowCount() == _p->commandDescriptors.size(),
        {{"bindingEditors", bindingEditors.size()}, {"choicesPerEditor", inputModel ? inputModel->rowCount() : 0},
         {"modelInstances", dialog->findChildren<QStandardItemModel*>(QStringLiteral("SettingsInputCommandModel")).size()},
         {"shortcutEditors", dialog->findChildren<QKeySequenceEdit*>().size()}});

    const QString shortcutId = QStringLiteral("playback.nextFrame");
    auto* shortcutEditor = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("SettingsShortcut_") + shortcutId);
    auto* saveShortcuts = dialog->findChild<QPushButton*>(QStringLiteral("SettingsSaveShortcuts"));
    auto* restoreShortcuts = dialog->findChild<QPushButton*>(QStringLiteral("SettingsRestoreShortcuts"));
    auto* nextFrameButton = findChild<QToolButton*>(QStringLiteral("PlaybackBarNextFrame"));
    QHash<QString, QString> originalShortcuts;
    QHash<QString, QVariant> savedShortcutSettings;
    QHash<QString, QVariant> savedImportSettings;
    for (const auto& descriptor : _p->commandDescriptors) originalShortcuts.insert(descriptor.id, descriptor.shortcut);
    for (const auto& key : _p->userSettings->allKeys()) {
        savedImportSettings.insert(key, _p->userSettings->value(key));
        if (key.startsWith(QStringLiteral("shortcuts/"))) savedShortcutSettings.insert(key, _p->userSettings->value(key));
    }
    const auto shortcutMatches = [&](const QString& expected) {
        const QKeySequence sequence(expected);
        bool descriptorMatched = false;
        for (const auto& descriptor : _p->commandDescriptors)
            if (descriptor.id == shortcutId) descriptorMatched = QKeySequence(descriptor.shortcut) == sequence;
        bool tableMatched = false;
        if (table) for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0) && table->item(row, 0)->text() == shortcutId && table->item(row, 2))
                tableMatched = QKeySequence(table->item(row, 2)->text()) == sequence;
        const auto actions = findChildren<QAction*>(shortcutId);
        bool actionsMatched = !actions.isEmpty();
        for (auto* action : actions) actionsMatched = actionsMatched && action->shortcut() == sequence;
        return shortcutEditor && shortcutEditor->keySequence() == sequence && descriptorMatched && tableMatched && actionsMatched;
    };
    const QString remappedSequence = QStringLiteral("Ctrl+Alt+Shift+F12");
    const QString nativeRemapped = QKeySequence(remappedSequence).toString(QKeySequence::NativeText);
    const QString shortcutKey = QStringLiteral("shortcuts/") + shortcutId;
    const auto emptyShortcutHintsLocalized = [&] {
        for (auto* edit : dialog->findChildren<QKeySequenceEdit*>()) {
            auto* line = edit->findChild<QLineEdit*>();
            if (edit->keySequence().isEmpty() && (!line || line->placeholderText() != QStringLiteral("点击设置快捷键"))) return false;
        }
        return true;
    };
    const auto tooltipHasSequence = [&](const QString& sequence) {
        return nextFrameButton && nextFrameButton->toolTip().contains(QKeySequence(sequence).toString(QKeySequence::NativeText));
    };
    bool remapPassed = shortcutEditor && saveShortcuts && restoreShortcuts && nextFrameButton;
    bool conflictRejected = false;
    bool clearPassed = false;
    bool restorePassed = false;
    bool stickyPassed = false;
    settingsClickItem(navigation, 3);
    settleSettings();
    if (remapPassed) {
        auto* scroll = tabs->widget(3)->findChild<QScrollArea*>();
        if (scroll) {
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            settleSettings();
            const QRect saveRect(saveShortcuts->mapTo(dialog, QPoint()), saveShortcuts->size());
            const QRect restoreRect(restoreShortcuts->mapTo(dialog, QPoint()), restoreShortcuts->size());
            stickyPassed = saveShortcuts->isVisible() && restoreShortcuts->isVisible() &&
                dialog->rect().contains(saveRect) && dialog->rect().contains(restoreRect);
        }
        shortcutEditor->setKeySequence(QKeySequence(remappedSequence));
        saveShortcuts->click();
        remapPassed = shortcutMatches(remappedSequence) && tooltipHasSequence(remappedSequence) && emptyShortcutHintsLocalized() &&
            _p->userSettings->value(shortcutKey).toString() == remappedSequence;
        auto* previousEditor = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("SettingsShortcut_playback.previousFrame"));
        if (previousEditor && !previousEditor->keySequence().isEmpty()) {
            shortcutEditor->setKeySequence(previousEditor->keySequence());
            saveShortcuts->click();
            conflictRejected = _p->userSettings->value(shortcutKey).toString() == remappedSequence &&
                tooltipHasSequence(remappedSequence);
        }
        shortcutEditor->clear();
        saveShortcuts->click();
        clearPassed = shortcutMatches(QString()) && _p->userSettings->contains(shortcutKey) &&
            _p->userSettings->value(shortcutKey).toString().isEmpty() && !nextFrameButton->toolTip().contains(nativeRemapped) &&
            nextFrameButton->toolTip().contains(QStringLiteral("快捷键：未设置")) &&
            !nextFrameButton->accessibleDescription().contains(nativeRemapped) && emptyShortcutHintsLocalized();
        restoreShortcuts->click();
        const QString defaultSequence = _p->commandDefaultShortcuts.value(shortcutId);
        restorePassed = shortcutMatches(defaultSequence) && tooltipHasSequence(defaultSequence) && emptyShortcutHintsLocalized();
        for (const auto& descriptor : _p->commandDescriptors) {
            const auto* edit = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("SettingsShortcut_") + descriptor.id);
            if (descriptor.id.isEmpty()) continue;
            restorePassed = restorePassed && !_p->userSettings->contains(QStringLiteral("shortcuts/") + descriptor.id) &&
                descriptor.shortcut == _p->commandDefaultShortcuts.value(descriptor.id) && edit &&
                edit->keySequence() == QKeySequence(descriptor.shortcut);
        }
    }
    const QString shortcutsCapture = outputDir.filePath(QStringLiteral("settings_shortcuts_sticky_actions.png"));
    const bool shortcutsSaved = dialog->grab().save(shortcutsCapture);
    add(QStringLiteral("Settings shortcut remap clear restore and presentation"),
        remapPassed && conflictRejected && clearPassed && restorePassed && stickyPassed && shortcutsSaved,
        {{"command", shortcutId}, {"remapSynchronized", remapPassed}, {"conflictRejected", conflictRejected},
         {"explicitEmptySynchronized", clearPassed}, {"allDefaultsRestored", restorePassed},
         {"stickyActionsVisible", stickyPassed}, {"capture", shortcutsCapture}});

    auto* importProfile = dialog->findChild<QPushButton*>(QStringLiteral("SettingsImportProfile"));
    const QString importPath = outputDir.filePath(QStringLiteral("settings_shortcut_import_fixture.json"));
    QSaveFile importFile(importPath);
    const QJsonObject importFixture{{"version", 3}, {"shortcuts", QJsonObject{{shortcutId, remappedSequence}}}};
    const QByteArray importBytes = QJsonDocument(importFixture).toJson(QJsonDocument::Indented);
    const bool fixtureSaved = importFile.open(QIODevice::WriteOnly) && importFile.write(importBytes) == importBytes.size() &&
        importFile.commit();
    if (fixtureSaved && importProfile) {
        settingsClickItem(navigation, 2);
        qApp->setProperty("cgplay.automationSettingsImportPath", importPath);
        importProfile->click();
        qApp->setProperty("cgplay.automationSettingsImportPath", QVariant());
    }
    add(QStringLiteral("Settings imported shortcut refreshes all presentations"), fixtureSaved && importProfile &&
        shortcutMatches(remappedSequence) && tooltipHasSequence(remappedSequence) && emptyShortcutHintsLocalized() &&
        _p->userSettings->value(shortcutKey).toString() == remappedSequence,
        {{"fixture", importPath}, {"command", shortcutId}, {"deterministicBackgroundImport", true}});

    // Restore the isolated test namespace exactly, including absent versus
    // explicitly empty bindings, and drive the same production save refresh.
    for (auto it = originalShortcuts.cbegin(); it != originalShortcuts.cend(); ++it)
        if (auto* edit = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("SettingsShortcut_") + it.key()))
            edit->setKeySequence(QKeySequence(it.value()));
    if (saveShortcuts) saveShortcuts->click();
    // Import also normalizes optional profile sections. Restore all isolated
    // settings keys, not just the one shortcut used for this regression.
    for (const auto& key : _p->userSettings->allKeys()) _p->userSettings->remove(key);
    for (auto it = savedImportSettings.cbegin(); it != savedImportSettings.cend(); ++it)
        _p->userSettings->setValue(it.key(), it.value());
    _p->userSettings->sync();
    bool shortcutSnapshotRestored = shortcutMatches(originalShortcuts.value(shortcutId));
    QJsonArray normalizedShortcutSpellings;
    QJsonArray mismatchedShortcutBindings;
    for (const auto& descriptor : _p->commandDescriptors) {
        const QString before = originalShortcuts.value(descriptor.id);
        const bool sameBinding = QKeySequence(descriptor.shortcut) == QKeySequence(before);
        // Qt's editor serializes Delete as Del. Require the exact key binding,
        // not the legacy display spelling; persisted settings remain exact.
        shortcutSnapshotRestored = shortcutSnapshotRestored && sameBinding;
        if (!sameBinding) mismatchedShortcutBindings.append(QJsonObject{{"command", descriptor.id}, {"before", before}, {"after", descriptor.shortcut}});
        else if (descriptor.shortcut != before)
            normalizedShortcutSpellings.append(QJsonObject{{"command", descriptor.id}, {"before", before}, {"after", descriptor.shortcut}});
    }
    QHash<QString, QVariant> restoredShortcutSettings;
    for (const auto& key : _p->userSettings->allKeys())
        if (key.startsWith(QStringLiteral("shortcuts/"))) restoredShortcutSettings.insert(key, _p->userSettings->value(key));
    add(QStringLiteral("Settings shortcut test values restored"), shortcutSnapshotRestored &&
        restoredShortcutSettings == savedShortcutSettings,
        {{"restoredCommandCount", originalShortcuts.size()}, {"bindingMismatches", mismatchedShortcutBindings},
         {"normalizedSpellings", normalizedShortcutSpellings}, {"persistedSnapshotExact", restoredShortcutSettings == savedShortcutSettings}});

    bool shortcutRecorderPassed = shortcutEditor != nullptr;
    if (shortcutEditor) {
        const QKeySequence before = shortcutEditor->keySequence();
        searchFor(QStringLiteral("内存缓存"));
        auto* recorder = shortcutEditor->findChild<QLineEdit*>();
        settingsKey(recorder ? static_cast<QWidget*>(recorder) : shortcutEditor, Qt::Key_F, Qt::ControlModifier);
        shortcutRecorderPassed = shortcutEditor->keySequence() == QKeySequence(QStringLiteral("Ctrl+F")) &&
            search->text() == QStringLiteral("内存缓存");
        const QSignalBlocker blocker(shortcutEditor);
        shortcutEditor->setKeySequence(before);
        searchFor(QString());
    }
    add(QStringLiteral("Settings shortcut recording keeps Ctrl F"), shortcutRecorderPassed);

    QHash<QSlider*, int> sliderValues;
    QJsonArray ranges;
    bool defaultsPassed = true;
    QSlider* opacity = nullptr;
    for (auto* slider : dialog->findChildren<QSlider*>()) {
        const QString key = slider->property("cgplay.appearanceKey").toString();
        if (key.isEmpty()) continue;
        sliderValues.insert(slider, slider->value());
        const int fallback = slider->property("cgplay.appearanceFallback").toInt();
        const int expected = _p->userSettings->value(key, fallback).toInt();
        defaultsPassed = defaultsPassed && LiveSlider::logicalValue(slider->value()) == expected;
        ranges.append(QJsonObject{{"key", key}, {"minimum", LiveSlider::logicalValue(slider->minimum())},
            {"maximum", LiveSlider::logicalValue(slider->maximum())}, {"value", LiveSlider::logicalValue(slider->value())}});
        if (key == QStringLiteral("appearance/buttonOpacity")) opacity = slider;
    }
    add(QStringLiteral("Settings existing slider values"), defaultsPassed && sliderValues.size() == 13,
        {{"sliders", ranges}});

    const QString modeKey = QStringLiteral("appearance/mode");
    const QString opacityKey = QStringLiteral("appearance/buttonOpacity");
    const bool hadMode = _p->userSettings->contains(modeKey);
    const bool hadOpacity = _p->userSettings->contains(opacityKey);
    const QVariant oldMode = _p->userSettings->value(modeKey);
    const QVariant oldOpacity = _p->userSettings->value(opacityKey);
    QComboBox* themeMode = nullptr;
    for (auto* combo : tabs->widget(1)->findChildren<QComboBox*>())
        if (combo->findData(QStringLiteral("dark")) >= 0 && combo->findData(QStringLiteral("light")) >= 0 &&
            combo->findData(QStringLiteral("custom")) >= 0) themeMode = combo;
    const int originalThemeIndex = themeMode ? themeMode->currentIndex() : -1;
    tabs->setCurrentIndex(1);
    bool previewPassed = opacity != nullptr;
    double dragMs = 0.0;
    if (opacity) {
        if (auto* scroll = qobject_cast<QScrollArea*>(tabs->widget(1))) scroll->ensureWidgetVisible(opacity);
        settleSettings();
        const QPoint start(opacity->width() / 4, opacity->height() / 2);
        const QPoint end(opacity->width() * 3 / 4, opacity->height() / 2);
        timer.restart();
        settingsMouse(opacity, QEvent::MouseButtonPress, start);
        for (int step = 1; step <= 40; ++step)
            settingsMouse(opacity, QEvent::MouseMove, start + (end - start) * step / 40);
        const int previewValue = LiveSlider::logicalValue(opacity->value());
        const bool notCommittedDuringDrag = _p->userSettings->contains(opacityKey) == hadOpacity &&
            _p->userSettings->value(opacityKey) == oldOpacity;
        previewPassed = opacity->isSliderDown() && notCommittedDuringDrag &&
            qApp->property("cgplay.buttonOpacity").toInt() == previewValue;
        settingsMouse(opacity, QEvent::MouseButtonRelease, end);
        dragMs = timer.nsecsElapsed() / 1e6;
        previewPassed = previewPassed && !opacity->isSliderDown() &&
            _p->userSettings->value(opacityKey).toInt() == LiveSlider::logicalValue(opacity->value()) &&
            qApp->property("cgplay.appearanceRefreshPending").toBool();
    }
    add(QStringLiteral("Settings slider preview and commit"), previewPassed,
        {{"mouseMoveEvents", 40}, {"dispatchMs", dragMs}});

    QJsonArray themeCaptures;
    bool themePassed = themeMode != nullptr;
    if (themeMode) {
        for (const QString& mode : {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("custom")}) {
            themeMode->setCurrentIndex(themeMode->findData(mode));
            settleSettings();
            const QString capture = outputDir.filePath(QStringLiteral("settings_theme_%1.png").arg(mode));
            const bool saved = dialog->grab().save(capture);
            themePassed = themePassed && saved && qApp->property("cgplay.themeMode").toString() == mode &&
                dialog->styleSheet() == qApp->property("cgplay.settingsDialogStyleSheet").toString() &&
                dialog->styleSheet().contains(qApp->property("cgplay.dialogColor").toString());
            themeCaptures.append(QJsonObject{{"theme", mode}, {"capture", capture}});
        }
    }
    add(QStringLiteral("Settings theme changes remain live"), themePassed, {{"captures", themeCaptures}});

    const QSize originalSize = dialog->size();
    QJsonArray responsiveCaptures;
    QJsonArray clippedControls;
    bool responsivePassed = true;
    int horizontalControlsChecked = 0;
    for (const QSize size : {QSize(760, 600), QSize(900, 720)}) {
        dialog->resize(size);
        for (int page = 0; page < tabs->count(); ++page) {
            settingsClickItem(navigation, page);
            settleSettings();
            const QRect navigationRect(navigation->mapTo(dialog, QPoint()), navigation->size());
            const QRect pagesRect(tabs->mapTo(dialog, QPoint()), tabs->size());
            const QRect searchRect(search->mapTo(dialog, QPoint()), search->size());
            const bool geometryGood = dialog->size() == size && dialog->rect().contains(navigationRect) && dialog->rect().contains(pagesRect) &&
                dialog->rect().contains(searchRect) && navigationRect.right() < pagesRect.left() &&
                searchRect.bottom() < pagesRect.top() && pagesRect.width() >= 400;
            QSet<QWidget*> fields;
            for (auto* form : tabs->widget(page)->findChildren<QFormLayout*>()) {
                for (int row = 0; row < form->rowCount(); ++row) {
                    auto* item = form->itemAt(row, QFormLayout::FieldRole);
                    if (item && item->widget()) fields.insert(item->widget());
                }
            }
            for (auto* widget : tabs->widget(page)->findChildren<QWidget*>()) {
                if (!qobject_cast<QPushButton*>(widget) && !qobject_cast<QCheckBox*>(widget) &&
                    !qobject_cast<QComboBox*>(widget) && !qobject_cast<QKeySequenceEdit*>(widget) &&
                    !qobject_cast<QAbstractSpinBox*>(widget) && !qobject_cast<QSlider*>(widget) &&
                    !qobject_cast<QTableWidget*>(widget)) continue;
                bool internalEditorControl = false;
                for (auto* parent = widget->parentWidget(); parent && parent != tabs->widget(page); parent = parent->parentWidget()) {
                    if (qobject_cast<QComboBox*>(parent) || qobject_cast<QAbstractSpinBox*>(parent) ||
                        qobject_cast<QKeySequenceEdit*>(parent) || qobject_cast<QTableWidget*>(parent)) {
                        internalEditorControl = true;
                        break;
                    }
                }
                if (!internalEditorControl) fields.insert(widget);
            }
            for (auto* field : fields) {
                if (!field->isVisible()) continue;
                QScrollArea* containingScroll = nullptr;
                for (auto* parent = field->parentWidget(); parent; parent = parent->parentWidget())
                    if ((containingScroll = qobject_cast<QScrollArea*>(parent))) break;
                if (!containingScroll) continue;
                ++horizontalControlsChecked;
                const QRect fieldRect(field->mapTo(containingScroll->viewport(), QPoint()), field->size());
                const QRect viewportRect = containingScroll->viewport()->rect();
                // Vertical scrolling is expected; horizontal cropping is not.
                if (fieldRect.left() < viewportRect.left() || fieldRect.right() > viewportRect.right()) {
                    responsivePassed = false;
                    clippedControls.append(QJsonObject{{"width", size.width()}, {"page", page},
                        {"objectName", field->objectName()}, {"type", QString::fromLatin1(field->metaObject()->className())},
                        {"fieldLeft", fieldRect.left()}, {"fieldRight", fieldRect.right()}, {"viewportWidth", viewportRect.width()}});
                }
            }
            const QString capture = outputDir.filePath(QStringLiteral("settings_%1x%2_page_%3.png")
                .arg(size.width()).arg(size.height()).arg(page));
            const bool saved = dialog->grab().save(capture);
            responsivePassed = responsivePassed && geometryGood && saved;
            responsiveCaptures.append(QJsonObject{{"width", dialog->width()}, {"height", dialog->height()},
                {"requestedWidth", size.width()}, {"requestedHeight", size.height()},
                {"page", page}, {"geometryWithinDialog", geometryGood}, {"capture", capture}});
        }
    }
    dialog->resize(originalSize);
    add(QStringLiteral("Settings narrow and standard layout captures"), responsivePassed,
        {{"captures", responsiveCaptures}, {"horizontalControlsChecked", horizontalControlsChecked},
         {"clippedControls", clippedControls}, {"captureMethod", "QWidget::grab (background surface)"}});

    const auto widgetCount = dialog->findChildren<QWidget*>().size();
    dialog->reject();
    settleSettings();
    const bool closedCleanly = !dialog->isVisible() && !qApp->property("cgplay.deferAppearanceRefresh").toBool() &&
        !qApp->property("cgplay.appearanceRefreshPending").toBool();
    timer.restart();
    _installSettingsWidgets(_p->reviewPanel);
    const double reopenMs = timer.nsecsElapsed() / 1e6;
    const bool correctWizard = dynamic_cast<AIConnectionWizardWidget*>(
        dialog->findChild<QWidget*>(QStringLiteral("AIConnectionWizardWidget"))) != nullptr;
    qApp->setProperty("cgplay.deferAppearanceRefresh", true);
    dialog->show();
    settleSettings();
    auto* closeBox = dialog->findChild<QDialogButtonBox*>();
    if (closeBox && closeBox->button(QDialogButtonBox::Close)) closeBox->button(QDialogButtonBox::Close)->click();
    else dialog->reject();
    settleSettings();
    add(QStringLiteral("Settings reuse and close cleanup"), closedCleanly && correctWizard && _p->settingsDialog == dialog &&
        dialog->findChildren<QWidget*>().size() == widgetCount && !dialog->isVisible() &&
        !qApp->property("cgplay.deferAppearanceRefresh").toBool(),
        {{"reopenDispatchMs", reopenMs}, {"wizardTypeVerified", correctWizard}, {"widgetCountBefore", widgetCount},
         {"widgetCountAfter", dialog->findChildren<QWidget*>().size()}});

    if (hadMode) _p->userSettings->setValue(modeKey, oldMode); else _p->userSettings->remove(modeKey);
    if (hadOpacity) _p->userSettings->setValue(opacityKey, oldOpacity); else _p->userSettings->remove(opacityKey);
    _p->userSettings->sync();
    if (themeMode) { const QSignalBlocker blocker(themeMode); themeMode->setCurrentIndex(originalThemeIndex); }
    for (auto it = sliderValues.cbegin(); it != sliderValues.cend(); ++it) {
        const QSignalBlocker blocker(it.key());
        it.key()->setValue(it.value());
    }
    tabs->setCurrentIndex(0);
    if (auto* app = qobject_cast<Application*>(qApp)) app->refreshAppearanceSettings();
    settleSettings();
    add(QStringLiteral("Settings isolated values restored"), _p->userSettings->contains(modeKey) == hadMode &&
        _p->userSettings->contains(opacityKey) == hadOpacity && _p->userSettings->value(modeKey) == oldMode &&
        _p->userSettings->value(opacityKey) == oldOpacity);
    return results;
}

} // namespace cgplay
