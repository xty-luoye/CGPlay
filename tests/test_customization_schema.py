"""Focused regression checks for the persisted customization contract.

These checks are intentionally file-level: they run without Qt and catch schema
drift in profile export/import and custom toolbar validation.
"""
import json
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
APPLICATION_OWNER_FILES = (
    "ApplicationInternal.h",
    "ApplicationUiSupport.cpp",
    "Application.cpp",
    "ApplicationSettings.cpp",
    "ApplicationMedia.cpp",
    "ApplicationSubtitles.cpp",
    "ApplicationEnhancement.cpp",
    "ApplicationInput.cpp",
)


class _ApplicationOwners:
    """Compatibility view over the ownership-split Application implementation."""

    def read_text(self, encoding="utf-8"):
        app_dir = ROOT / "src" / "ui" / "app"
        return "\n".join(
            (app_dir / filename).read_text(encoding=encoding)
            for filename in APPLICATION_OWNER_FILES
        )


APPLICATION_CPP = _ApplicationOwners()


class CustomizationSchemaTests(unittest.TestCase):
    def test_profile_parts_and_atomic_writes_are_present(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        for part in ("theme.json", "shortcuts.json", "toolbar.json", "button_styles.json", "workspace.json", "metadata.json"):
            self.assertIn(part, source)
        self.assertIn("QSaveFile", source)

    def test_custom_toolbar_button_contract(self):
        button = {
            "id": "custom.capture_annotate",
            "name": "截图并批注",
            "icon": "camera-plus",
            "group": "自定义按键",
            "commands": ["codex.captureFrame", "annotation.create"],
        }
        self.assertRegex(button["id"], r"^[a-z0-9_.-]+$")
        self.assertGreaterEqual(len(button["commands"]), 1)
        self.assertTrue(all(isinstance(command, str) and command for command in button["commands"]))
        self.assertEqual(button, json.loads(json.dumps(button, ensure_ascii=False)))

    def test_unknown_command_is_rejected_by_source_guard(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        self.assertIn("未知命令", source)
        self.assertIn("known.contains(command)", source)

    def test_opacity_sliders_track_live_and_commit_on_release(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        self.assertGreaterEqual(source.count("setTracking(true)"), 3)
        self.assertGreaterEqual(source.count("&QSlider::sliderReleased"), 3)
        self.assertIn('setValue(QStringLiteral("appearance/buttonOpacity"), logical)', source)
        self.assertNotIn('setValue(QStringLiteral("appearance/buttonOpacity"), logical); _p->userSettings->sync()', source)
        self.assertIn("&QSlider::valueChanged", source)

    def test_live_slider_and_isolated_capture_dump_modes(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        self.assertIn("class LiveSlider", source)
        self.assertIn("class SettingsScrollArea", source)
        self.assertIn("WM_MOUSEWHEEL", source)
        self.assertIn("WM_MBUTTONDOWN", source)
        self.assertIn("SetCapture", source)
        self.assertIn("if (eventBelongsToSettings) return false", source)
        self.assertIn("setSmoothRange", source)
        self.assertIn("kSmoothScale = 10", source)
        self.assertIn("setValue(valueFromPoint", source)
        self.assertIn("grabMouse()", source)
        self.assertIn("releaseMouse()", source)
        self.assertIn("setSliderDown(true)", source)
        self.assertIn("setSliderDown(false)", source)
        self.assertIn("void wheelEvent(QWheelEvent* event) override", source)
        self.assertIn("eventBelongsToMainWindow", source)
        self.assertIn("eventBelongsToSettings", source)
        self.assertIn("settingsScrollAt", source)
        self.assertIn("Qt::MiddleButton", source)
        self.assertIn("settingsScrollStartValue - deltaY", source)
        self.assertIn("qRound(angleDelta * 0.5)", source)
        self.assertIn("settingsDragGrabWidget->grabMouse", source)
        self.assertIn('qApp->setProperty("cgplay.appearanceRefreshPending", true)', source)
        self.assertIn("previewAppearanceValue", source)
        self.assertNotIn("for (QWidget* widget : findChildren<QWidget*>())", source)
        self.assertIn("_captureUiMode || _dumpRuntimeMode", runtime)

    def test_background_image_is_owned_by_shell_not_video_surface(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        viewport = (ROOT / "src" / "ui" / "viewer" / "TlViewport.cpp").read_text(encoding="utf-8")
        codex = (ROOT / "src" / "plugins" / "codex" / "CodexAgentWorkspace.cpp").read_text(encoding="utf-8")
        self.assertIn('const QString configuredImagePath = appStringProperty("cgplay.backgroundImage")', source)
        self.assertIn("drawBackdropPixmapAligned", source)
        self.assertNotIn("drawViewerRuntimeBackdrop(painter, rect())", viewport)
        self.assertNotIn("border-image:url", codex)
        self.assertIn("definition.panel, definition.panelOpacity", runtime)

    def test_review_panel_does_not_stack_translucent_surfaces(self):
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        review = (ROOT / "src" / "features" / "annotation" / "ReviewPanel.cpp").read_text(encoding="utf-8")
        self.assertIn('QWidget#ReviewPanel{background:transparent', runtime)
        self.assertIn('QTabWidget::pane{border:none;background:transparent', runtime)
        self.assertNotIn('for (auto* frame : review->findChildren<QFrame*>())', runtime)
        self.assertIn('glass.setColorAt(0.0, panel.lighter', review)
        self.assertNotIn('qRound(186 * panelOpacity', review)

    def test_top_bar_has_only_one_full_surface_layer(self):
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        top_bar = (ROOT / "src" / "ui" / "app" / "TopBar.cpp").read_text(encoding="utf-8")
        self.assertIn('item.first == QStringLiteral("cgplayTopBarSurface")', runtime)
        self.assertNotIn('QLinearGradient sheen', top_bar)
        self.assertIn('painter.fillRect(rect(), base)', top_bar)

    def test_viewer_chrome_uses_aligned_translucent_shell_backdrop(self):
        viewer = (ROOT / "src" / "ui" / "viewer" / "ViewerWidget.cpp").read_text(encoding="utf-8")
        renderer = (ROOT / "src" / "common" / "theme" / "BackdropRenderer.cpp").read_text(encoding="utf-8")
        self.assertIn('drawRuntimeBackdrop(painter, rect(), this)', viewer)
        self.assertIn('surface->mapTo(root, QPoint(0, 0))', renderer)
        self.assertIn('applicationInt("cgplay.backgroundBrightness", 100)', renderer)
        self.assertIn('applicationInt("cgplay.backgroundSaturation", 100)', renderer)
        self.assertIn('applicationInt("cgplay.backgroundBlurRadius", 0)', renderer)
        self.assertIn('applicationInt("cgplay.backgroundVignette", 0)', renderer)
        self.assertIn('runtimeInt("cgplay.toolbarOpacity", 92)', viewer)
        self.assertIn('colorCss(toolbarSurface)', viewer)

    def test_timeline_opacity_invalidates_static_layer_cache(self):
        timeline = (ROOT / "src" / "features" / "timeline" / "TimelineWidget.cpp").read_text(encoding="utf-8")
        signature_start = timeline.index("const QString themeSignature")
        signature_end = timeline.index("if (_themeSignature != themeSignature)", signature_start)
        signature = timeline[signature_start:signature_end]
        self.assertIn('property("cgplay.timelineOpacity")', signature)

    def test_codex_uses_shared_processed_backdrop_cache(self):
        codex = (ROOT / "src" / "plugins" / "codex" / "CodexAgentWorkspace.cpp").read_text(encoding="utf-8")
        renderer = (ROOT / "src" / "common" / "theme" / "BackdropRenderer.cpp").read_text(encoding="utf-8")
        self.assertIn('drawApplicationBackdrop(painter, rect(), this, backdropOptions)', codex)
        self.assertNotIn('const QPixmap source(path)', codex)
        self.assertIn('info.lastModified().toMSecsSinceEpoch()', renderer)
        self.assertIn('.arg(target.width()).arg(target.height())', renderer)
        self.assertIn('reader.setScaledSize(decodeSize)', renderer)

    def test_compare_and_annotation_toolbars_follow_runtime_theme(self):
        compare = (ROOT / "src" / "ui" / "viewer" / "CompareToolbar.cpp").read_text(encoding="utf-8")
        annotation = (ROOT / "src" / "features" / "annotation" / "AnnotationToolbar.cpp").read_text(encoding="utf-8")
        for source in (compare, annotation):
            self.assertIn('themeColor("cgplay.toolbarColor"', source)
            self.assertIn('themeColor("cgplay.panelColor"', source)
            self.assertIn('themeColor("cgplay.textColor"', source)
            self.assertIn('themeColor("cgplay.borderColor"', source)
            self.assertIn('themeColor("cgplay.accentColor"', source)
            self.assertIn('&QApplication::paletteChanged', source)
        self.assertNotIn('setStyleSheet("background: #0d1520;")', compare)
        self.assertNotIn('background:rgba(10,14,18,0.50)', annotation)
        self.assertNotIn('const QPixmap source(path)', compare + annotation)

    def test_codex_full_size_containers_do_not_stack_panel_layers(self):
        codex = (ROOT / "src" / "plugins" / "codex" / "CodexAgentWorkspace.cpp").read_text(encoding="utf-8")
        for object_name in (
            "CodexAgentWorkspace", "CodexContent", "CodexSourceSummary",
            "CodexConversation", "CodexConversationViewport",
            "CodexConversationHost", "CodexComposerWrap", "CodexDiagnostics",
        ):
            self.assertIn(f'#{object_name}{{background:transparent', codex)
        self.assertIn('glass.setColorAt(0.0, panel.lighter', codex)
        self.assertIn('QLinearGradient topEdge', codex)

    def test_all_customization_commands_are_registered(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        for command in (
            "annotation.create", "annotation.delete", "codex.captureFrame",
            "codex.browserSnapshot", "export.render", "workspace.save",
            "workspace.reset", "playback.setSpeed", "playback.loop",
            "translation.mode",
        ):
            self.assertIn(f'QStringLiteral("{command}")', source)

    def test_button_style_has_disabled_state_and_preset_isolation(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        viewer = (ROOT / "src" / "ui" / "viewer" / "ViewerWidget.cpp").read_text(encoding="utf-8")
        playback = (ROOT / "src" / "ui" / "app" / "PlaybackBar.cpp").read_text(encoding="utf-8")
        self.assertIn("buttonDisabled", runtime)
        self.assertIn("QToolButton:disabled", runtime)
        self.assertIn("mode != cgplay::ThemeMode::Custom", runtime)
        self.assertIn("findChildren<QAbstractButton*>()", source)
        self.assertIn("buttonStyles.insert(id, style)", source)
        self.assertIn('QStringLiteral("appearance/%1/%2")', source)
        self.assertIn("buttonDisabled\"), QStringLiteral(\"buttonText", source)
        self.assertIn('applyButtonStyle(button, styleId)', runtime)
        self.assertIn('cgplay.customStyleApplied', runtime)
        self.assertIn('cgplay.customStyleApplied', viewer)
        for command in (
            "playback.previousFrame", "playback.toggle", "playback.nextFrame",
            "translation.toggle", "translation.mode", "playback.setSpeed",
            "playback.loop", "audio.toggleMute",
        ):
            self.assertIn(f'QStringLiteral("{command}")', playback)

    def test_settings_editors_defer_expensive_work(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        self.assertIn('cgplay.deferAppearanceRefresh', runtime)
        self.assertIn('cgplay.appearanceRefreshPending', runtime)
        self.assertGreaterEqual(source.count("setKeyboardTracking(false)"), 4)
        self.assertIn("windowOpacityLabel = new QSpinBox", source)
        self.assertIn("windowOpacityPreviewTimer->start(16)", source)
        self.assertIn("applyPlayerWindowOpacity(this, logical)", source)
        self.assertIn("SetLayeredWindowAttributes", source)
        self.assertIn("buttonOpacityLabel = new QSpinBox", source)
        self.assertIn("connect(ramCacheSpin, &QSpinBox::editingFinished", source)
        self.assertIn("connect(aheadSpin, &QSpinBox::editingFinished", source)
        self.assertIn("connect(behindSpin, &QSpinBox::editingFinished", source)
        self.assertNotIn("connect(editor, &QSpinBox::valueChanged", source)

    def test_restore_all_customization_is_scoped_and_confirmed(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        self.assertIn('QStringLiteral("恢复所有自定义设置")', source)
        self.assertIn("QMessageBox::Yes | QMessageBox::Cancel", source)
        for prefix in ("appearance/", "shortcuts/", "toolbar/", "workspace/", "input/"):
            self.assertIn(f'QStringLiteral("{prefix}")', source)
        self.assertNotIn("_p->userSettings->clear()", source)
        self.assertIn("_p->commandDefaultShortcuts.value(descriptor.id)", source)
        self.assertIn("_p->inputBindings = std::make_unique<InputBindingStore>()", source)

    def test_per_button_editor_is_removed_from_settings_ui(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        self.assertNotIn('if (false) {', source)
        self.assertNotIn("buttonStyleGroup", source)
        self.assertIn("Legacy per-button profile keys remain readable", source)

    def test_codex_send_and_error_styles_are_stable(self):
        source = (ROOT / "src" / "plugins" / "codex" / "CodexAgentWorkspace.cpp").read_text(encoding="utf-8")
        self.assertIn('setObjectName(QStringLiteral("CodexSend"))', source)
        self.assertIn("min-width:31px;max-width:31px", source)
        self.assertIn('setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed)', source)
        self.assertIn('CodexError{color:%6', source)
        self.assertIn('appThemeColor("cgplay.textColor"', source)

    def test_codex_controls_use_readable_text_colors(self):
        source = (ROOT / "src" / "plugins" / "codex" / "CodexAgentWorkspace.cpp").read_text(encoding="utf-8")
        self.assertIn('const QColor muted = mixColors(text, panel, 0.28);', source)
        self.assertIn('"#CodexHeader QToolButton{color:%6;}"', source)
        self.assertIn('"#CodexHeaderNewConversation,#CodexSessions,#CodexOverflow,#CodexClose', source)
        self.assertIn('"#CodexEmpty{color:%6;', source)
        self.assertIn('"#CodexWho{color:%6;', source)
        self.assertIn('"QComboBox QAbstractItemView{background:%2;color:%6;', source)
        self.assertIn('QComboBox#CodexModel QAbstractItemView', source)
        self.assertIn('popup->setStyleSheet', source)
        self.assertIn('#CodexHeaderNewConversation:disabled', source)
        self.assertIn('#CodexSend:disabled{background:%3;color:%6', source)
        self.assertIn('const QList<QToolButton*> materialButtons', source)
        self.assertIn('buttonPalette.setColor(QPalette::Disabled, QPalette::Button', source)
        self.assertIn('const QString headerStyle = QStringLiteral', source)
        self.assertIn('const QString composerStyle = QStringLiteral', source)
        self.assertIn('const QString chromeText = QStringLiteral("#F4F7FB")', source)
        self.assertIn('QMenu::item:disabled{color:%5', source)
        self.assertIn('popupPalette.setColor(QPalette::HighlightedText', source)
        self.assertNotIn('"QToolButton{border:1px solid transparent;border-radius:6px;color:%7;', source)

    def test_codex_default_dock_matches_released_layout(self):
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        self.assertIn("codexDock->setMinimumWidth(460)", runtime)
        self.assertIn("codexDock->setMaximumWidth(520)", runtime)
        self.assertIn("codexDock->setBaseSize(460, 0)", runtime)
        self.assertIn("resizeDocks({codexDock}, {460}", runtime)
        self.assertNotIn("codexDock->setMaximumWidth(340)", runtime)

    def test_volume_control_uses_click_popup_slider(self):
        playback = (ROOT / "src" / "ui" / "app" / "PlaybackBar.cpp").read_text(encoding="utf-8")
        self.assertIn('PlaybackBarVolumePopup', playback)
        self.assertIn('Qt::Popup | Qt::FramelessWindowHint', playback)
        self.assertIn('setAttribute(Qt::WA_TranslucentBackground, true)', playback)
        self.assertIn('class VolumePopupWidget final : public QWidget', playback)
        self.assertIn('painter.setBrush(QColor(18, 23, 29, 230))', playback)
        self.assertIn('painter.drawRoundedRect', playback)
        self.assertNotIn('PlaybackBarVolumePopup{background:rgba(', playback)

    def test_playback_bar_volume_popup_does_not_override_global_opacity(self):
        playback = (ROOT / "src" / "ui" / "app" / "PlaybackBar.cpp").read_text(encoding="utf-8")
        self.assertIn('qBound(0, qApp->property("cgplay.toolbarOpacity").toInt(), 100)', playback)
        self.assertIn('volumePopup->show()', playback)
        self.assertIn('connect(mutePopupButton, &QToolButton::clicked', playback)
        self.assertIn('_playback->setMute(mutePopupButton->isChecked())', playback)
        self.assertIn('_btnMute->setCheckable(false)', playback)
        self.assertNotIn('_btnMute->setChecked(', playback)
        self.assertIn('mutePopupButton->setChecked(_playback && _playback->isMuted())', playback)
        self.assertIn('const QIcon stateIcon(renderVolumeIcon(muted))', playback)
        self.assertIn('_btnMute->setIcon(stateIcon)', playback)
        self.assertIn('cgplay.originalIcon', playback)
        self.assertIn('QColor("#FF5B64")', playback)
        self.assertIn('cgplay.preserveStateIcon', playback)
        self.assertNotIn('connect(_btnMute, &QToolButton::clicked, this, [this] { _playback->toggleMute(); });', playback)
        self.assertNotIn('_volSlider->setVisible(availableWidth >= 740)', playback)
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        self.assertIn('QToolButton:checked,QPushButton:checked', runtime)

    def test_playback_toggle_changes_icon_without_checked_color_state(self):
        playback = (ROOT / "src" / "ui" / "app" / "PlaybackBar.cpp").read_text(encoding="utf-8")
        self.assertNotIn('_btnPlay->setCheckable(true)', playback)
        self.assertNotIn('_btnPlay->setChecked(', playback)
        self.assertIn('_btnPlay->setIcon(playStateIcon(playing, iconColor))', playback)

    def test_open_media_reuses_probe_for_playlist_and_thumbnail(self):
        app = APPLICATION_CPP.read_text(encoding="utf-8")
        playlist = (ROOT / "src" / "features" / "playlist" / "PlaylistModel.cpp").read_text(encoding="utf-8")
        thumbnails = (ROOT / "src" / "services" / "media" / "ThumbnailService.cpp").read_text(encoding="utf-8")
        self.assertIn('playlistModel->addPath(path, mediaInfo, sequenceFpsOverride)', app)
        self.assertIn('const MediaInfo* mediaInfo', playlist)
        self.assertIn('ThumbnailService::makePlaylistImage(', playlist)
        self.assertIn('JobRunner::start(', playlist)
        self.assertIn('const MediaInfo* mediaInfo', thumbnails)

    def test_version_and_component_check_does_not_refresh_manifest_twice(self):
        app = APPLICATION_CPP.read_text(encoding="utf-8")
        update = (ROOT / "src" / "ui" / "app" / "UpdateService.cpp").read_text(encoding="utf-8")
        self.assertIn('_checkForUpdates(interactive, false)', app)
        self.assertIn('if (refreshRemote)', update)

    def test_shortcuts_dispatch_through_mutable_command_descriptors(self):
        app = APPLICATION_CPP.read_text(encoding="utf-8")
        secondary = (ROOT / "src" / "ui" / "app" / "SecondaryWindow.cpp").read_text(encoding="utf-8")
        for command in (
            "playback.reverse", "playback.stop", "playback.forward",
            "playback.seekBackward10", "annotation.undo", "annotation.tool.rectangle",
            "compare.autoClearB",
        ):
            self.assertIn(f'QStringLiteral("{command}")', app)
        self.assertIn("for (const auto& descriptor : _p->commandDescriptors)", app)
        self.assertIn("descriptor.shortcut = sequence", app)
        self.assertNotIn("case Qt::Key_Space: _p->playbackCtrl->togglePlay()", app)
        self.assertNotIn("case Qt::Key_M: _p->playbackCtrl->toggleMute()", app)
        self.assertIn('QStringLiteral("shortcuts/%1")', secondary)
        self.assertNotIn("case Qt::Key_Space:", secondary)

    def test_command_semantics_and_full_input_surface(self):
        app = APPLICATION_CPP.read_text(encoding="utf-8")
        playback = (ROOT / "src" / "ui" / "app" / "PlaybackBar.cpp").read_text(encoding="utf-8")
        codex_header = (ROOT / "src" / "plugins" / "codex" / "CodexAgentWorkspace.h").read_text(encoding="utf-8")
        self.assertIn('setProperty("commandId", QStringLiteral("audio.openVolume"))', playback)
        self.assertIn('mutePopupButton->setProperty("commandId", QStringLiteral("audio.toggleMute"))', playback)
        self.assertIn("speed->showMenu()", app)
        self.assertIn("Q_INVOKABLE bool captureBrowserSnapshot()", codex_header)
        for gesture in ("A", "B", "X", "Y", "LB", "RB", "Back", "Start", "L3", "R3", "DPadUp", "DPadDown", "DPadLeft", "DPadRight"):
            self.assertIn(f'QStringLiteral("{gesture}")', app)

    def test_profile_validation_assets_and_responsive_layout_guards(self):
        app = APPLICATION_CPP.read_text(encoding="utf-8")
        topbar = (ROOT / "src" / "ui" / "app" / "TopBar.cpp").read_text(encoding="utf-8")
        self.assertIn("SettingsProfilePackage::fromJson", app)
        profile_service = (ROOT / "src" / "services" / "settings" / "SettingsProfileService.cpp").read_text(encoding="utf-8")
        self.assertIn("knownCommands.contains(id)", profile_service)
        self.assertIn("Shortcut is invalid or duplicated", profile_service)
        self.assertIn('QStringLiteral("assets/%1.%2")', app)
        self.assertIn('QStringLiteral("themes/assets")', app)
        self.assertIn("mode == 0 && showLeft && showRight", app)
        self.assertIn("available < 720", topbar)
        self.assertIn("TopBarStatusDivider", topbar)
        runtime = (ROOT / "src" / "ui" / "app" / "ApplicationRuntime.cpp").read_text(encoding="utf-8")
        self.assertIn('!widget->property("cgplay.preserveStateIcon").toBool()', runtime)
        controller = (ROOT / "src" / "core" / "playback" / "PlaybackController.cpp").read_text(encoding="utf-8")
        self.assertIn('#endif\n    Q_EMIT muteChanged(m);', controller)
        self.assertIn('#endif\n    Q_EMIT volumeChanged(_p->volume);', controller)
        self.assertIn('return _p->muted;', controller)
        self.assertIn('return _p->volume;', controller)

    def test_profile_rejects_unknown_commands_before_persisting_any_settings(self):
        app = APPLICATION_CPP.read_text(encoding="utf-8")
        import_start = app.index("connect(importProfile, &QPushButton::clicked")
        validation_start = app.index("QSet<QString> knownCommands", import_start)
        rejection = app.index("if (!packageValid)", validation_start)
        settings_apply = app.index("for (auto it = theme.begin();", rejection)
        import_body = app[validation_start:settings_apply]

        self.assertLess(validation_start, rejection)
        self.assertLess(rejection, settings_apply)
        self.assertIn("SettingsProfilePackage::fromJson", import_body)
        self.assertIn("profile, knownCommands, &packageValid, &validationError", import_body)
        self.assertIn("rejectProfile", import_body)
        self.assertIn("typedPackage.mouseBindings", app)
        self.assertIn("typedPackage.gamepadBindings", app)

    def test_zip_profile_packages_and_restores_both_theme_assets_safely(self):
        app = APPLICATION_CPP.read_text(encoding="utf-8")
        export_start = app.index('if (path.endsWith(QStringLiteral(".zip")')
        import_start = app.index("connect(importProfile, &QPushButton::clicked", export_start)
        export_body = app[export_start:import_start]
        apply_start = app.index("for (auto it = theme.begin();", import_start)
        import_body = app[import_start:apply_start]

        self.assertIn('packageThemeAsset(QStringLiteral("backgroundImage"), QStringLiteral("background"))', export_body)
        self.assertIn('packageThemeAsset(QStringLiteral("texturePath"), QStringLiteral("texture"))', export_body)
        self.assertIn('QStringLiteral("assets/%1.%2")', export_body)
        self.assertIn("packagedTheme.insert(key, relative)", export_body)
        self.assertIn('QStringLiteral("themes/assets")', import_body)
        self.assertIn('QStringLiteral("backgroundImage"), QStringLiteral("texturePath")', import_body)
        self.assertIn("source.canonicalFilePath()", import_body)
        self.assertIn("canonicalSource.startsWith(extractedRoot + QDir::separator()", import_body)
        self.assertIn("QSaveFile destinationFile(destination)", import_body)
        self.assertIn("theme.insert(key, destination)", import_body)

    def test_input_binding_store_uses_atomic_backup_and_corruption_recovery(self):
        store = (ROOT / "src" / "common" / "input" / "InputBindingStore.cpp").read_text(encoding="utf-8")
        load_start = store.index("bool InputBindingStore::load")
        save_start = store.index("bool InputBindingStore::save", load_start)
        binding_start = store.index("QString InputBindingStore::binding", save_start)
        load_body = store[load_start:save_start]
        save_body = store[save_start:binding_start]

        self.assertIn('QFile backup(primaryPath + QStringLiteral(".bak"))', load_body)
        self.assertIn("if (!backupDocument.isObject())", load_body)
        self.assertIn("doc = backupDocument", load_body)
        self.assertIn("QSaveFile recovered(primaryPath)", load_body)
        self.assertIn('const QString backupPath = filePath(device) + QStringLiteral(".bak")', save_body)
        self.assertIn("QFile::copy(filePath(device), backupPath)", save_body)
        self.assertIn("QSaveFile file(filePath(device))", save_body)
        self.assertIn("file.commit()", save_body)

    def test_profile_contains_all_input_binding_parts(self):
        app = APPLICATION_CPP.read_text(encoding="utf-8")
        for part in ("input.json", "mouse_bindings.json", "gamepad_bindings.json"):
            self.assertIn(f'QStringLiteral("{part}")', app)
        self.assertIn('{QStringLiteral("mouseBindings"), mouseBindings}', app)
        self.assertIn('{QStringLiteral("gamepadBindings"), gamepadBindings}', app)

    def test_side_panels_resize_continuously_without_auto_close(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        self.assertIn('_p->playlist->setMinimumWidth(0)', source)
        self.assertGreaterEqual(source.count('_p->reviewPanel->setMinimumWidth(0)'), 3)
        self.assertIn('_p->playlist->setSizePolicy(QSizePolicy::Ignored', source)
        self.assertIn(': _p->centerSplitter->mapToGlobal(QPoint(0, 0)).x()', source)
        self.assertIn('if (boundary == 1 && !_p->playlist->isVisible())', source)
        self.assertGreaterEqual(source.count('std::clamp(requested, 0, maximum)'), 2)
        self.assertNotIn('kSidePanelCollapseThreshold', source)
        self.assertIn('qBound(1, sizes[leftIndex], 520)', source)
        self.assertIn('qBound(1, sizes[rightIndex], 520)', source)
        self.assertIn('layout/leftPanelVisible', source)
        self.assertIn('layout/rightPanelVisible', source)

    def test_default_material_uses_opaque_reference_surfaces(self):
        theme = (ROOT / "src" / "common" / "theme" / "ThemeService.cpp").read_text(encoding="utf-8")
        self.assertIn("result.panelOpacity = 100", theme)
        self.assertIn("result.toolbarOpacity = 100", theme)
        self.assertIn("result.timelineOpacity = 100", theme)

    def test_settings_edits_defer_global_appearance_rebuild(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        refresh_start = source.index("const auto refreshAppearanceImmediately")
        refresh_end = source.index("const auto colorButtonText", refresh_start)
        refresh_body = source[refresh_start:refresh_end]
        self.assertIn("app->refreshAppearanceSettings()", refresh_body)
        self.assertIn('setProperty("cgplay.deferAppearanceRefresh", false)', refresh_body)
        self.assertIn("Discrete operations", refresh_body)
        self.assertIn('setProperty("cgplay.appearanceRefreshPending", true)', source)
        self.assertIn("if (refreshPending)", source)

    def test_side_panel_width_survives_hide_restore_and_workspace_save(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        private = (ROOT / "src" / "ui" / "app" / "ApplicationPrivate.h").read_text(encoding="utf-8")
        self.assertIn("lastLeftPanelWidth", private)
        self.assertIn("lastRightPanelWidth", private)
        self.assertIn("_p->lastLeftPanelWidth = qBound", source)
        self.assertIn("_p->lastRightPanelWidth = qBound", source)
        self.assertGreaterEqual(source.count(": _p->lastLeftPanelWidth"), 2)
        self.assertGreaterEqual(source.count(": _p->lastRightPanelWidth"), 2)
        self.assertIn("QEvent::UngrabMouse", source)
        self.assertIn("finishSettingsScrollDrag", source)
        self.assertIn("finishSidePanelDrag", source)

    def test_workspace_persists_codex_translation_and_secondary_window(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        self.assertIn('panels.insert(QStringLiteral("codex")', source)
        self.assertIn('workspacePanels.insert(QStringLiteral("codex")', source)
        self.assertGreaterEqual(source.count('QStringLiteral("translationEnabled")'), 5)
        self.assertGreaterEqual(source.count('QStringLiteral("secondaryWindow")'), 5)
        self.assertIn("applyWorkspaceExtras(workspace.toJson())", source)
        self.assertIn("_p->workspaceController->load", source)
        self.assertIn("_p->workspaceController->save", source)
        self.assertIn("app->openNewWindow(true)", source)

    def test_theme_presets_and_profile_archives_are_responsive(self):
        source = APPLICATION_CPP.read_text(encoding="utf-8")
        self.assertIn("ThemeService::save", source)
        self.assertIn("ThemeService::load", source)
        self.assertIn("ThemeService::remove", source)
        self.assertIn("runProcessResponsive", source)
        self.assertNotIn("waitForFinished(120000)", source)
        self.assertIn('const QString backupPath = path + QStringLiteral(".bak")', source)


if __name__ == "__main__":
    unittest.main()
