cmake_minimum_required(VERSION 3.21)

set(CGPLAY_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

set(CGPLAY_PLUGIN_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")

add_library(CGPlayTheme SHARED
    common/theme/BackdropRenderer.cpp
    common/theme/BackdropRenderer.h
)
target_compile_definitions(CGPlayTheme PRIVATE CGPLAY_THEME_LIBRARY)
target_include_directories(CGPlayTheme PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(CGPlayTheme PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)

option(CGPLAY_ENABLE_AGENT_CONSOLE "Build the isolated Agent Console scaffold" OFF)
option(CGPLAY_REQUIRE_RUNTIME
       "Fail configuration when bundled FFmpeg/Codex runtimes are missing"
       OFF)

set(CGPLAY_SOURCES
    main.cpp

    ui/app/Application.cpp
    ui/app/ApplicationEnhancement.cpp
    ui/app/ApplicationInput.cpp
    ui/app/ApplicationMedia.cpp
    ui/app/ApplicationSettings.cpp
    ui/app/ApplicationSubtitles.cpp
    ui/app/ApplicationUiSupport.cpp
    ui/app/ApplicationInternal.h
    ui/app/ApplicationRuntime.cpp
    ui/app/ApplicationRuntimeAutomation.cpp
    ui/app/ApplicationRuntimeSupport.cpp
    ui/app/MainWindowAutomation.cpp
    ui/app/MainWindowAutomationFullscreen.cpp
    ui/app/MainWindowAutomationMedia.cpp
    ui/app/MainWindowAutomationSettings.cpp
    ui/app/MainWindowAutomationSubtitles.cpp
    ui/app/Application.h
    ui/app/ApplicationPrivate.h
    ui/app/AIAgentWorkspace.cpp
    ui/app/AIAgentWorkspaceActions.cpp
    ui/app/AIAgentWorkspaceControl.cpp
    ui/app/AIAgentWorkspacePresentation.cpp
    ui/app/AIAgentWorkspaceSettings.cpp
    ui/app/AIAgentWorkspaceSupport.cpp
    ui/app/AIAgentWorkspace.h
    ui/app/AIAgentWorkspaceSupport.h
    ui/app/AIConnectionWizardWidget.cpp
    ui/app/AIConnectionWizardWidget.h
    ui/app/TitleBar.cpp
    ui/app/TitleBar.h
    ui/app/TopBar.cpp
    ui/app/TopBar.h
    ui/app/PerformanceService.cpp
    ui/app/PerformanceService.h
    ui/app/UpdateService.cpp
    ui/app/UpdateService.h
    ui/app/NavigationRail.cpp
    ui/app/NavigationRail.h
    ui/app/PlaybackBar.cpp
    ui/app/PlaybackBar.h
    ui/app/SecondaryWindow.cpp
    ui/app/SecondaryWindow.h

    common/core/ThreadPool.cpp
    common/core/ThreadPool.h
    common/core/AsyncImageLoader.cpp
    common/core/AsyncImageLoader.h
    common/core/GPUUploadQueue.cpp
    common/core/GPUUploadQueue.h
    common/core/CrashHandler.cpp
    common/core/CrashHandler.h
    common/core/PlaybackLogger.cpp
    common/core/PlaybackLogger.h
    common/core/ServiceLocator.h
    common/core/SystemPerformanceMonitor.cpp
    common/core/SystemPerformanceMonitor.h
    common/jobs/JobSystem.cpp
    common/jobs/JobSystem.h
    common/events/EventBus.cpp
    common/events/EventBus.h
    common/events/api/IEventBus.h
    common/events/api/EventTypes.h
    common/settings/SettingsService.cpp
    common/settings/SettingsService.h
    common/settings/api/ISettingsService.h
    common/settings/profiles/AppearanceProfile.cpp
    common/settings/profiles/AppearanceProfile.h
    common/settings/profiles/WorkspaceProfile.cpp
    common/settings/profiles/WorkspaceProfile.h
    common/settings/profiles/ToolbarProfile.cpp
    common/settings/profiles/ToolbarProfile.h
    services/settings/SettingsProfileService.cpp
    services/settings/SettingsProfileService.h
    services/settings/controllers/AppearanceController.cpp
    services/settings/controllers/AppearanceController.h
    services/settings/controllers/WorkspaceController.cpp
    services/settings/controllers/WorkspaceController.h
    services/settings/controllers/ToolbarController.cpp
    services/settings/controllers/ToolbarController.h
    common/theme/ThemeTypes.h
    common/theme/ThemeService.cpp
    common/theme/ThemeService.h
    common/input/InputBindingStore.cpp
    common/input/InputBindingStore.h

    plugins/PluginManager.cpp
    plugins/PluginManager.h
    plugins/CommandRegistry.cpp
    plugins/CommandRegistry.h
    ui/app/CommandDispatcher.cpp
    ui/app/CommandDispatcher.h
    plugins/api/IPlugin.h
    plugins/api/HostExtensionPoints.h
    plugins/api/ICommandProvider.h
    plugins/api/IMenuProvider.h
    plugins/api/IToolbarProvider.h
    plugins/api/IPanelProvider.h
    plugins/annotation/AnnotationPlugin.cpp
    plugins/annotation/AnnotationPlugin.h
    plugins/ocio/OcioPlugin.cpp
    plugins/ocio/OcioPlugin.h
    plugins/quicklook/QuickLookPlugin.cpp
    plugins/quicklook/QuickLookPlugin.h

    services/component/ComponentManager.cpp
    services/component/ComponentManager.h
    services/component/DownloadService.cpp
    services/component/DownloadService.h
    services/ai/api/AIProviderTypes.h
    services/ai/api/IAICredentialStore.h
    services/ai/api/IAIContextBuilder.h
    services/ai/api/IAIProvider.h
    services/ai/api/IAIProviderDetector.h
    services/ai/api/IAIProviderManager.h
    services/ai/api/IAIWorkflowService.h
    services/ai/api/IMediaFrameSnapshotService.h
    services/ai/AIContextBuilder.cpp
    services/ai/AIContextBuilder.h
    services/ai/AIProviderManager.cpp
    services/ai/AIProviderManager.h
    services/ai/AIWorkflowService.cpp
    services/ai/AIWorkflowService.h
    services/ai/MediaFrameSnapshotService.cpp
    services/ai/MediaFrameSnapshotService.h
    services/ai/OpenAIResponsesProvider.cpp
    services/ai/OpenAIResponsesProvider.h
    services/ai/SubtitleAsrApiClient.cpp
    services/ai/SubtitleAsrApiClient.h
    services/ai/SubtitleGenerationService.cpp
    services/ai/SubtitleGenerationService.h
    services/ai/SubtitleGenerationSupport.cpp
    services/ai/SubtitleGenerationSupport.h
    services/ai/SubtitleCredentialMigration.cpp
    services/ai/SubtitleCredentialMigration.h
    services/ai/TranslationBaselineController.cpp
    services/ai/TranslationBaselineController.h
    services/ai/TranslationFusionController.cpp
    services/ai/TranslationFusionController.h
    services/ai/TranslationGuardSkills.cpp
    services/ai/TranslationGuardSkills.h
    services/ai/TranslationEnhancementScheduler.cpp
    services/ai/TranslationEnhancementScheduler.h
    services/ai/TranslationEnhancementSupport.cpp
    services/ai/TranslationEnhancementSupport.h
    services/ai/TranslationPlaybackStrategy.cpp
    services/ai/TranslationPlaybackStrategy.h
    services/ai/TranslationSkillRegistry.cpp
    services/ai/TranslationSkillRegistry.h
    services/ai/TranslationSpeedGuard.cpp
    services/ai/TranslationSpeedGuard.h
    services/ai/TranslationTextPolicy.cpp
    services/ai/TranslationTextPolicy.h
    services/ai/WindowsDpapiCredentialStore.cpp
    services/ai/WindowsDpapiCredentialStore.h
    services/ai/discovery/AIConnectionValidator.cpp
    services/ai/discovery/AIConnectionValidator.h
    services/ai/discovery/AIModelDiscovery.cpp
    services/ai/discovery/AIModelDiscovery.h
    services/ai/discovery/AIProviderDetector.cpp
    services/ai/discovery/AIProviderDetector.h

    ui/viewer/TlViewport.cpp
    ui/viewer/TlViewport.h
    ui/viewer/ViewerWidget.cpp
    ui/viewer/ViewerWidget.h
    ui/viewer/api/IActivePlaybackView.h
    ui/viewer/api/IOverlayHost.h
    ui/viewer/api/IOverlayProvider.h
    ui/viewer/api/IViewerCoordinateMapper.h
    ui/viewer/CompareToolbar.cpp
    ui/viewer/CompareToolbar.h

    core/playback/PlaybackController.cpp
    core/playback/PlaybackController.h
    core/playback/PlaybackStats.cpp
    core/playback/PlaybackStats.h
    core/playback/api/PlaybackBackend.h
    core/playback/api/IPlaybackService.h
    core/playback/api/PlaybackServiceSignals.h

    features/timeline/TimelineWidget.cpp
    features/timeline/TimelineWidget.h

    services/media/MediaService.cpp
    services/media/MediaService.h
    services/media/api/IMediaService.h
    services/media/MediaProbe.cpp
    services/media/MediaProbe.h
    services/media/ThumbnailService.cpp
    services/media/ThumbnailService.h
    services/media/ThumbnailPipeline.cpp
    services/media/ThumbnailPipeline.h

    features/playlist/PlaylistModel.cpp
    features/playlist/PlaylistModel.h
    features/playlist/PlaylistPanel.cpp
    features/playlist/PlaylistPanel.h

    services/ocio/OcioManager.cpp
    services/ocio/OcioManager.h

    core/cache/CacheManager.cpp
    core/cache/CacheManager.h
    core/cache/ReadAheadCache.cpp
    core/cache/ReadAheadCache.h

    features/annotation/AnnotationItem.h
    features/annotation/api/IAnnotationService.h
    features/annotation/api/IAnnotationViewBridge.h
    features/annotation/AnnotationManager.cpp
    features/annotation/AnnotationManager.h
    features/annotation/AnnotationOverlay.cpp
    features/annotation/AnnotationOverlay.h
    features/annotation/AnnotationOverlayProvider.cpp
    features/annotation/AnnotationOverlayProvider.h
    features/annotation/AnnotationToolbar.cpp
    features/annotation/AnnotationToolbar.h
    features/annotation/ReviewPanel.cpp
    features/annotation/ReviewPanel.h
    features/annotation/AnnotationStorage.cpp
    features/annotation/AnnotationStorage.h
    features/annotation/ReviewExport.cpp
    features/annotation/ReviewExport.h

    features/otio/OtioImporter.cpp
    features/otio/OtioImporter.h
    features/otio/OtioExporter.cpp
    features/otio/OtioExporter.h

    core/session/SessionManager.cpp
    core/session/SessionManager.h
    core/session/api/ISessionService.h
    core/session/api/ISessionContributor.h

    core/hwdecode/HardwareDecodeManager.cpp
    core/hwdecode/HardwareDecodeManager.h

    services/platform/WindowsFileAssociations.cpp
    services/platform/WindowsFileAssociations.h
)

set(CGPLAY_QUICKLOOK_SOURCES
    plugins/quicklook/main_quicklook.cpp
    plugins/quicklook/QuickLookApp.cpp
    plugins/quicklook/QuickLookApp.h
    plugins/quicklook/PreviewWindow.cpp
    plugins/quicklook/PreviewWindow.h
    plugins/quicklook/ExplorerSelection.cpp
    plugins/quicklook/ExplorerSelection.h
    plugins/quicklook/QuickLookDebug.cpp
    plugins/quicklook/QuickLookDebug.h
    plugins/quicklook/QuickLookKeyboardHook.cpp
    plugins/quicklook/QuickLookKeyboardHook.h

    ui/viewer/TlViewport.cpp
    ui/viewer/TlViewport.h
    ui/viewer/ViewerWidget.cpp
    ui/viewer/ViewerWidget.h

    features/annotation/AnnotationOverlay.cpp
    features/annotation/AnnotationOverlay.h

    core/playback/PlaybackController.cpp
    core/playback/PlaybackController.h
    core/playback/PlaybackStats.cpp
    core/playback/PlaybackStats.h
    core/playback/api/PlaybackServiceSignals.h

    services/ocio/OcioManager.cpp
    services/ocio/OcioManager.h

    core/cache/CacheManager.cpp
    core/cache/CacheManager.h
    core/cache/ReadAheadCache.cpp
    core/cache/ReadAheadCache.h

    common/core/ThreadPool.cpp
    common/core/ThreadPool.h
    common/core/AsyncImageLoader.cpp
    common/core/AsyncImageLoader.h
    common/core/GPUUploadQueue.cpp
    common/core/GPUUploadQueue.h
    common/core/SystemPerformanceMonitor.cpp
    common/core/SystemPerformanceMonitor.h
    common/jobs/JobSystem.cpp
    common/jobs/JobSystem.h

    services/component/ComponentManager.cpp
    services/component/ComponentManager.h
    services/component/DownloadService.cpp
    services/component/DownloadService.h

)

if(CGPLAY_ENABLE_AGENT_CONSOLE)
    list(APPEND CGPLAY_SOURCES
        services/agent/AgentHost.cpp
        services/agent/AgentHost.h
        ui/app/AgentConsole.cpp
        ui/app/AgentConsole.h
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../resources/resources.qrc")
    list(APPEND CGPLAY_SOURCES ../resources/resources.qrc)
    list(APPEND CGPLAY_QUICKLOOK_SOURCES ../resources/resources.qrc)
endif()

if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../resources/CGPlay.rc")
    list(APPEND CGPLAY_SOURCES ../resources/CGPlay.rc)
    list(APPEND CGPLAY_QUICKLOOK_SOURCES ../resources/CGPlay.rc)
endif()

add_executable(CGPlay WIN32 ${CGPLAY_SOURCES})
add_executable(CGPlayQuickLook WIN32 ${CGPLAY_QUICKLOOK_SOURCES})

if(WIN32)
    add_library(CGPlayThumbnailProvider SHARED
        shell/CGPlayThumbnailProvider.cpp
        shell/CGPlayThumbnailProvider.h
        shell/CGPlayThumbnailProvider.def
        ../resources/CGPlay.rc
    )
    target_compile_definitions(CGPlayThumbnailProvider PRIVATE CGPLAY_THUMBNAIL_PROVIDER_BUILD)
    target_include_directories(CGPlayThumbnailProvider PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(CGPlayThumbnailProvider PRIVATE ole32 shlwapi windowscodecs uuid)
    target_compile_options(CGPlayThumbnailProvider PRIVATE /utf-8)
    set_target_properties(CGPlayThumbnailProvider PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CGPlayThumbnailProvider"
    )
endif()

add_library(CGPlayAnnotationPlugin MODULE
    plugins/annotation/AnnotationPlugin.cpp
    plugins/annotation/AnnotationPlugin.h
    features/annotation/api/IAnnotationService.h
    features/annotation/api/IAnnotationViewBridge.h
    features/annotation/AnnotationManager.cpp
    features/annotation/AnnotationManager.h
    features/annotation/AnnotationOverlay.cpp
    features/annotation/AnnotationOverlay.h
    features/annotation/AnnotationOverlayProvider.cpp
    features/annotation/AnnotationOverlayProvider.h
    features/annotation/AnnotationToolbar.cpp
    features/annotation/AnnotationToolbar.h
    features/annotation/ReviewPanel.cpp
    features/annotation/ReviewPanel.h
    core/session/api/ISessionContributor.h
    core/playback/api/PlaybackServiceSignals.h
    ui/viewer/api/IActivePlaybackView.h
    ui/viewer/api/IOverlayHost.h
    ui/viewer/api/IOverlayProvider.h
    ui/viewer/api/IViewerCoordinateMapper.h
)
add_library(CGPlayOcioPlugin MODULE
    plugins/ocio/OcioPlugin.cpp
    plugins/ocio/OcioPlugin.h
)
add_library(CGPlayQuickLookPlugin MODULE
    plugins/quicklook/QuickLookPlugin.cpp
    plugins/quicklook/QuickLookPlugin.h
)
add_library(CGPlayCodexPlugin MODULE
    plugins/codex/CodexPlugin.cpp
    plugins/codex/CodexPlugin.h
    plugins/codex/CodexPluginSupport.cpp
    plugins/codex/CodexPluginSupport.h
    plugins/codex/CodexPluginTools.cpp
    plugins/codex/CodexPluginWorkspace.cpp
    plugins/codex/CodexAgentWorkspace.cpp
    plugins/codex/CodexAgentWorkspace.h
    plugins/codex/LocalCodexOrchestrator.cpp
    plugins/codex/LocalCodexOrchestrator.h
    services/agent/CodexAppServerSession.cpp
    services/agent/CodexAppServerSession.h
    common/jobs/JobSystem.cpp
    common/jobs/JobSystem.h
)

add_dependencies(CGPlay
    CGPlayAnnotationPlugin
    CGPlayOcioPlugin
    CGPlayQuickLookPlugin
    CGPlayCodexPlugin
)

set(CGPLAY_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/features"
    "${CMAKE_CURRENT_SOURCE_DIR}/services"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins"
)

target_include_directories(CGPlay PRIVATE ${CGPLAY_INCLUDE_DIRS})
target_include_directories(CGPlayQuickLook PRIVATE ${CGPLAY_INCLUDE_DIRS})
target_include_directories(CGPlayAnnotationPlugin PRIVATE ${CGPLAY_INCLUDE_DIRS})
target_include_directories(CGPlayOcioPlugin PRIVATE ${CGPLAY_INCLUDE_DIRS})
target_include_directories(CGPlayQuickLookPlugin PRIVATE ${CGPLAY_INCLUDE_DIRS})
target_include_directories(CGPlayCodexPlugin PRIVATE ${CGPLAY_INCLUDE_DIRS})

set(TLRENDER_INCLUDE_HINTS
    "${TLRENDER_SOURCE_DIR}/lib"
    "${FEATHER_TK_SOURCE_DIR}/lib"
    "${TLRENDER_INSTALL_DIR}/include"
    "${TLRENDER_INSTALL_DIR}/include/opentimelineio"
    "${TLRENDER_INSTALL_DIR}/include/opentime"
    "${TLRENDER_INSTALL_DIR}/share/opentimelineio"
)
foreach(hint IN LISTS TLRENDER_INCLUDE_HINTS)
    if(EXISTS "${hint}")
        target_include_directories(CGPlay PRIVATE "${hint}")
        target_include_directories(CGPlayQuickLook PRIVATE "${hint}")
    endif()
endforeach()

target_link_libraries(CGPlay PRIVATE
    CGPlayTheme
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
    Qt6::Network
    OpenGL::GL
)
target_link_libraries(CGPlayQuickLook PRIVATE
    CGPlayTheme
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
    Qt6::Network
    OpenGL::GL
    ole32
    oleaut32
)

target_link_libraries(CGPlayAnnotationPlugin PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
target_link_libraries(CGPlayOcioPlugin PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent)
target_link_libraries(CGPlayQuickLookPlugin PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent Qt6::Network OpenGL::GL)
target_link_libraries(CGPlayCodexPlugin PRIVATE CGPlayTheme Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::WebEngineWidgets)
if(WIN32)
    target_link_libraries(CGPlayCodexPlugin PRIVATE crypt32)
endif()

target_compile_definitions(CGPlayAnnotationPlugin PRIVATE CGPLAY_BUILD_QT_PLUGIN)
target_compile_definitions(CGPlayOcioPlugin PRIVATE CGPLAY_BUILD_QT_PLUGIN)
target_compile_definitions(CGPlayQuickLookPlugin PRIVATE CGPLAY_BUILD_QT_PLUGIN)
target_compile_definitions(CGPlayCodexPlugin PRIVATE CGPLAY_BUILD_QT_PLUGIN)

if(CGPLAY_USE_TLRENDER)
    foreach(tl_target
            tlRender::tlTimeline  tlRender::tlIO  tlRender::tlGL
            tlRender::tlCore  tlRender::tlDevice  tlRender::tlResource  tlRender::tlUI
            tlTimeline  tlIO  tlGL  tlCore  tlDevice  tlResource  tlUI)
        if(TARGET ${tl_target})
            target_link_libraries(CGPlay PRIVATE ${tl_target})
            target_link_libraries(CGPlayQuickLook PRIVATE ${tl_target})
        endif()
    endforeach()

    foreach(ftk_target ftk::ftkCore ftk::ftkGL ftk::ftkUI ftkCore ftkGL ftkUI)
        if(TARGET ${ftk_target})
            target_link_libraries(CGPlay PRIVATE ${ftk_target})
            target_link_libraries(CGPlayQuickLook PRIVATE ${ftk_target})
        endif()
    endforeach()

    target_compile_definitions(CGPlay PRIVATE CGPLAY_HAS_TLRENDER=1 CGPLAY_HAS_FTK=1 TL_STATIC)
    target_compile_definitions(CGPlayQuickLook PRIVATE CGPLAY_HAS_TLRENDER=1 CGPLAY_HAS_FTK=1 TL_STATIC)

    if(TARGET SDL2::SDL2)
        target_link_libraries(CGPlay PRIVATE SDL2::SDL2)
        target_link_libraries(CGPlayQuickLook PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(CGPlay PRIVATE SDL2::SDL2-static)
        target_link_libraries(CGPlayQuickLook PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(CGPlay PRIVATE SDL2)
        target_link_libraries(CGPlayQuickLook PRIVATE SDL2)
    endif()
endif()

if(CGPLAY_USE_OCIO AND OpenColorIO_FOUND)
    target_link_libraries(CGPlay PRIVATE OpenColorIO::OpenColorIO)
    target_link_libraries(CGPlayQuickLook PRIVATE OpenColorIO::OpenColorIO)
endif()

if(CGPLAY_USE_OCIO AND NOT OpenColorIO_FOUND)
    set(_ocio_lib "${TLRENDER_INSTALL_DIR}/lib/OpenColorIO.lib")
    if(EXISTS "${_ocio_lib}")
        target_link_libraries(CGPlay PRIVATE "${_ocio_lib}")
        target_link_libraries(CGPlayQuickLook PRIVATE "${_ocio_lib}")
        target_compile_definitions(CGPlay PRIVATE CGPLAY_OCIO_ENABLED)
        target_compile_definitions(CGPlayQuickLook PRIVATE CGPLAY_OCIO_ENABLED)
        message(STATUS "Using OCIO from tlRender install: ${_ocio_lib}")
    endif()
endif()

if(OpenEXR_FOUND)
    target_link_libraries(CGPlay PRIVATE OpenEXR::OpenEXR)
elseif(CGPLAY_USE_EXR)
    set(_exr_lib_vcpkg "${CGPLAY_VCPKG_LIBRARY_DIR}")
    set(_exr_lib_tlr   "${TLRENDER_INSTALL_DIR}/lib")
    if(EXISTS "${_exr_lib_vcpkg}/OpenEXR-3_4.lib")
        target_link_libraries(CGPlay PRIVATE
            "${_exr_lib_vcpkg}/OpenEXR-3_4.lib"
            "${_exr_lib_vcpkg}/OpenEXRCore-3_4.lib"
            "${_exr_lib_vcpkg}/OpenEXRUtil-3_4.lib"
            "${_exr_lib_vcpkg}/Iex-3_4.lib"
            "${_exr_lib_vcpkg}/IlmThread-3_4.lib"
        )
        target_link_libraries(CGPlayQuickLook PRIVATE
            "${_exr_lib_vcpkg}/OpenEXR-3_4.lib"
            "${_exr_lib_vcpkg}/OpenEXRCore-3_4.lib"
            "${_exr_lib_vcpkg}/OpenEXRUtil-3_4.lib"
            "${_exr_lib_vcpkg}/Iex-3_4.lib"
            "${_exr_lib_vcpkg}/IlmThread-3_4.lib"
        )
        message(STATUS "Using OpenEXR from vcpkg (3.4)")
    else()
        target_link_libraries(CGPlay PRIVATE
            "${_exr_lib_tlr}/OpenEXR-3_3.lib"
            "${_exr_lib_tlr}/OpenEXRCore-3_3.lib"
            "${_exr_lib_tlr}/OpenEXRUtil-3_3.lib"
            "${_exr_lib_tlr}/Iex-3_3.lib"
            "${_exr_lib_tlr}/IlmThread-3_3.lib"
        )
        target_link_libraries(CGPlayQuickLook PRIVATE
            "${_exr_lib_tlr}/OpenEXR-3_3.lib"
            "${_exr_lib_tlr}/OpenEXRCore-3_3.lib"
            "${_exr_lib_tlr}/OpenEXRUtil-3_3.lib"
            "${_exr_lib_tlr}/Iex-3_3.lib"
            "${_exr_lib_tlr}/IlmThread-3_3.lib"
        )
        message(STATUS "Using OpenEXR from tlRender install (3.3)")
    endif()
endif()

if(CGPLAY_USE_OTIO AND OpenTimelineIO_FOUND)
    if(TARGET OTIO::opentimelineio)
        target_include_directories(CGPlay PRIVATE "${TLRENDER_INSTALL_DIR}/include")
        target_include_directories(CGPlayQuickLook PRIVATE "${TLRENDER_INSTALL_DIR}/include")
        target_link_libraries(CGPlay PRIVATE OTIO::opentimelineio)
        target_link_libraries(CGPlayQuickLook PRIVATE OTIO::opentimelineio)
    elseif(TARGET OpenTimelineIO::opentimelineio)
        target_include_directories(CGPlay PRIVATE "${TLRENDER_INSTALL_DIR}/include")
        target_include_directories(CGPlayQuickLook PRIVATE "${TLRENDER_INSTALL_DIR}/include")
        target_link_libraries(CGPlay PRIVATE OpenTimelineIO::opentimelineio)
        target_link_libraries(CGPlayQuickLook PRIVATE OpenTimelineIO::opentimelineio)
    else()
        target_include_directories(CGPlay PRIVATE "${TLRENDER_INSTALL_DIR}/include")
        target_link_libraries(CGPlay PRIVATE ${OpenTimelineIO_LIBRARIES})
        target_include_directories(CGPlayQuickLook PRIVATE "${TLRENDER_INSTALL_DIR}/include")
        target_link_libraries(CGPlayQuickLook PRIVATE ${OpenTimelineIO_LIBRARIES})
    endif()
    target_compile_definitions(CGPlay PRIVATE CGPLAY_OTIO_ENABLED OTIO_STATIC)
    target_compile_definitions(CGPlayQuickLook PRIVATE CGPLAY_OTIO_ENABLED OTIO_STATIC)
endif()

if(NOT CGPLAY_USE_TLRENDER)
    target_sources(CGPlay PRIVATE common/core/TlStubs.cpp)
    set(_ftk_install_lib "${TLRENDER_INSTALL_DIR}/lib")
    if(EXISTS "${_ftk_install_lib}")
        target_link_libraries(CGPlay PRIVATE
            "${_ftk_install_lib}/ftkCore.lib"
            "${_ftk_install_lib}/ftkGL.lib"
            "${_ftk_install_lib}/ftkUI.lib"
            "${_ftk_install_lib}/ftkResource.lib"
            "${_ftk_install_lib}/ftkglad.lib"
            "${_ftk_install_lib}/freetype.lib"
            "${_ftk_install_lib}/Imath-3_2.lib"
            "${_ftk_install_lib}/Iex-3_3.lib"
            "${_ftk_install_lib}/IlmThread-3_3.lib"
            "${_ftk_install_lib}/minizip-ng.lib"
            "${_ftk_install_lib}/zlib.lib"
            "${_ftk_install_lib}/turbojpeg-static.lib"
            "${_ftk_install_lib}/libpng16_static.lib"
            "${_ftk_install_lib}/lunasvg.lib"
            "${_ftk_install_lib}/plutovg.lib"
            "${_ftk_install_lib}/nfd.lib"
            "${_ftk_install_lib}/pystring.lib"
            "${_ftk_install_lib}/yaml-cpp.lib"
            "${_ftk_install_lib}/libexpatMD.lib"
            "${_ftk_install_lib}/tiff.lib"
            "${_ftk_install_lib}/tiffxx.lib"
            "${_ftk_install_lib}/jpeg-static.lib"
            "${_ftk_install_lib}/SDL2-static.lib"
        )
        set(_tlr_build_lib "${TLRENDER_BUILD_DIR}/lib/tlRender")
        target_link_libraries(CGPlay PRIVATE
            "${_tlr_build_lib}/Core/Release/tlCore.lib"
            "${_tlr_build_lib}/IO/Release/tlIO.lib"
            "${_tlr_build_lib}/Resource/Release/tlResource.lib"
        )
        target_link_libraries(CGPlay PRIVATE winmm ole32 imm32 version)
        target_compile_definitions(CGPlay PRIVATE FTK_STATIC FTK_API_GL_4_1)
    endif()
endif()

if(WIN32)
    target_link_libraries(CGPlay PRIVATE pdh)
    target_link_libraries(CGPlay PRIVATE psapi)
    target_link_libraries(CGPlay PRIVATE crypt32)
    target_link_libraries(CGPlay PRIVATE windowsapp)
    target_link_libraries(CGPlay PRIVATE advapi32 shell32 shlwapi)
    target_link_libraries(CGPlayQuickLook PRIVATE pdh)
    target_link_libraries(CGPlayAnnotationPlugin PRIVATE pdh)
    target_link_libraries(CGPlayOcioPlugin PRIVATE pdh)
    target_link_libraries(CGPlayQuickLookPlugin PRIVATE pdh)

    set_target_properties(CGPlay PROPERTIES
        WIN32_EXECUTABLE TRUE
        VS_DPI_AWARE "PerMonitor"
    )
    set_target_properties(CGPlayQuickLook PROPERTIES
        WIN32_EXECUTABLE TRUE
        VS_DPI_AWARE "PerMonitor"
    )
    target_compile_options(CGPlay PRIVATE /utf-8)
    target_compile_options(CGPlayQuickLook PRIVATE /utf-8)
    target_compile_options(CGPlayAnnotationPlugin PRIVATE /utf-8)
    target_compile_options(CGPlayOcioPlugin PRIVATE /utf-8)
    target_compile_options(CGPlayQuickLookPlugin PRIVATE /utf-8)
    target_compile_options(CGPlayCodexPlugin PRIVATE /utf-8)
endif()

if(WIN32)
    set(_cgplay_ffmpeg_source_dir "${CMAKE_BINARY_DIR}/runtime/ffmpeg")
    set(_cgplay_codex_runtime_dir "${CMAKE_BINARY_DIR}/runtime/codex")
    set(_cgplay_codex_runtime_files
        codex.exe
        codex-code-mode-host.exe
        LICENSE-OPENAI-CODEX.txt
    )

    set(_cgplay_runtime_complete TRUE)
    set(_cgplay_runtime_missing)
    foreach(_cgplay_ffmpeg_file ffmpeg.exe ffprobe.exe)
        if(NOT EXISTS "${_cgplay_ffmpeg_source_dir}/${_cgplay_ffmpeg_file}")
            set(_cgplay_runtime_complete FALSE)
            list(APPEND _cgplay_runtime_missing
                "${_cgplay_ffmpeg_source_dir}/${_cgplay_ffmpeg_file}")
        endif()
    endforeach()
    foreach(_cgplay_codex_file IN LISTS _cgplay_codex_runtime_files)
        if(NOT EXISTS "${_cgplay_codex_runtime_dir}/${_cgplay_codex_file}")
            set(_cgplay_runtime_complete FALSE)
            list(APPEND _cgplay_runtime_missing
                "${_cgplay_codex_runtime_dir}/${_cgplay_codex_file}")
        endif()
    endforeach()

    if(NOT _cgplay_runtime_complete)
        string(JOIN ";" _cgplay_runtime_missing_text ${_cgplay_runtime_missing})
        if(CGPLAY_REQUIRE_RUNTIME)
            message(FATAL_ERROR
                "CGPlay runtime is incomplete; missing: ${_cgplay_runtime_missing_text}")
        endif()
        message(WARNING
            "CGPlay runtime is not staged; runtime deployment will be skipped. "
            "Missing: ${_cgplay_runtime_missing_text}. "
            "Set CGPLAY_REQUIRE_RUNTIME=ON for a release/package build.")
    endif()

    if(_cgplay_runtime_complete)
        add_custom_target(CGPlayRuntimeDeploy
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:CGPlay>/runtime/codex"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cgplay_ffmpeg_source_dir}/ffmpeg.exe"
                "$<TARGET_FILE_DIR:CGPlay>/ffmpeg.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cgplay_ffmpeg_source_dir}/ffprobe.exe"
                "$<TARGET_FILE_DIR:CGPlay>/ffprobe.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cgplay_codex_runtime_dir}/codex.exe"
                "$<TARGET_FILE_DIR:CGPlay>/runtime/codex/codex.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cgplay_codex_runtime_dir}/codex-code-mode-host.exe"
                "$<TARGET_FILE_DIR:CGPlay>/runtime/codex/codex-code-mode-host.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cgplay_codex_runtime_dir}/LICENSE-OPENAI-CODEX.txt"
                "$<TARGET_FILE_DIR:CGPlay>/runtime/codex/LICENSE-OPENAI-CODEX.txt"
            COMMENT "Deploying required CGPlay FFmpeg and Codex runtimes"
            VERBATIM
        )
    else()
        add_custom_target(CGPlayRuntimeDeploy
            COMMAND ${CMAKE_COMMAND} -E echo
                "CGPlay runtime not staged; skipping runtime deployment"
            COMMENT "Skipping optional CGPlay runtime deployment"
            VERBATIM
        )
    endif()
    add_dependencies(CGPlay CGPlayRuntimeDeploy)

    set(_cgplay_tools_source_dir "${CGPLAY_SOURCE_ROOT}/../tools/cgplay")
    if(EXISTS "${_cgplay_tools_source_dir}/export/export_engine.py")
        add_custom_command(TARGET CGPlay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:CGPlay>/tools"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_cgplay_tools_source_dir}"
                "$<TARGET_FILE_DIR:CGPlay>/tools/cgplay"
            COMMENT "Copying CGPlay export tools to output directory"
        )
    endif()

    set(_cgplay_python_runtime_candidates
        "${CMAKE_BINARY_DIR}/package/full/CGPlay/runtime/python"
        "${CMAKE_BINARY_DIR}/package/CGPlay/runtime/python"
    )
    foreach(_cgplay_python_runtime_dir IN LISTS _cgplay_python_runtime_candidates)
        if(EXISTS "${_cgplay_python_runtime_dir}/python.exe"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/certifi/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/charset_normalizer/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/email/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/http/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/idna/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/requests/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/tokenizers/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/faster_whisper/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/ctranslate2/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/tqdm/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/urllib3/__init__.py"
           AND EXISTS "${_cgplay_python_runtime_dir}/Lib/site-packages/typing_extensions.py")
            add_custom_command(TARGET CGPlay POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:CGPlay>/runtime"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_cgplay_python_runtime_dir}"
                    "$<TARGET_FILE_DIR:CGPlay>/runtime/python"
                COMMENT "Copying bundled Python runtime to output directory"
            )
            break()
        endif()
    endforeach()

    set(_cgplay_component_manifest
        "${CGPLAY_SOURCE_ROOT}/../resources/component_manifest.json")
    if(EXISTS "${_cgplay_component_manifest}")
        add_custom_command(TARGET CGPlay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:CGPlay>/resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cgplay_component_manifest}"
                "$<TARGET_FILE_DIR:CGPlay>/resources/component_manifest.json"
            COMMENT "Copying component manifest to output directory"
        )
    endif()

    add_custom_command(TARGET CGPlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:CGPlay>/presets"
        COMMENT "Ensuring presets directory exists"
    )
endif()

if(APPLE)
    set_target_properties(CGPlay PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.cgplay.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

if(WIN32 AND TARGET Qt6::Concurrent)
    get_target_property(_qt6concurrent_loc Qt6::Concurrent IMPORTED_LOCATION_RELEASE)
    if(NOT _qt6concurrent_loc)
        get_target_property(_qt6concurrent_loc Qt6::Concurrent IMPORTED_LOCATION)
    endif()
    if(_qt6concurrent_loc AND EXISTS "${_qt6concurrent_loc}")
        add_custom_command(TARGET CGPlay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_qt6concurrent_loc}"
                "$<TARGET_FILE_DIR:CGPlay>/Qt6Concurrent.dll"
            COMMENT "Copying Qt6Concurrent.dll to output directory"
        )
        add_custom_command(TARGET CGPlayQuickLook POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_qt6concurrent_loc}"
                "$<TARGET_FILE_DIR:CGPlayQuickLook>/Qt6Concurrent.dll"
            COMMENT "Copying Qt6Concurrent.dll to output directory"
        )
    endif()
endif()

foreach(_plugin_target CGPlayAnnotationPlugin CGPlayOcioPlugin CGPlayQuickLookPlugin CGPlayCodexPlugin)
    set_target_properties(${_plugin_target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CGPLAY_PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release/plugins"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug/plugins"
        RUNTIME_OUTPUT_DIRECTORY "${CGPLAY_PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release/plugins"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug/plugins"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    )
endforeach()
