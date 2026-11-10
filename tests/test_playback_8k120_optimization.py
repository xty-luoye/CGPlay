from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
class Playback8K120OptimizationTests(unittest.TestCase):
    def test_unvalidated_global_decode_overrides_are_absent(self):
        s=(ROOT/'src/core/playback/PlaybackController.cpp').read_text(encoding='utf-8')
        self.assertNotIn('readThreadCount = decodeThreads',s)
        self.assertNotIn('CGPLAY_ENABLE_HW_DECODE',s)
    def test_native_loader_version_is_defined_before_branch(self):
        s=(ROOT/'src/ui/viewer/NativeGLLoader.cpp').read_text(encoding='utf-8')
        self.assertLess(s.index('#include <tlRender/Core/Version.h>'),
                        s.index('#if TLRENDER_VERSION_MAJOR'))
    def test_viewport_preserves_processing_precision(self):
        s=(ROOT/'src/ui/viewer/TlViewport.cpp').read_text(encoding='utf-8')
        self.assertNotIn('useFloatBuffer',s)
        self.assertIn('TextureType::RGBA_F32',s)
    def test_benchmark_waits_for_async_open_and_scopes_callbacks(self):
        s=(ROOT/'src/ui/app/ApplicationRuntimeAutomation.cpp').read_text(encoding='utf-8')
        benchmark=s[s.index('int Application::_runPlaybackBenchmark()'):s.index('int Application::_runUiCapture()')]
        self.assertIn('openWait.elapsed() < 30000',benchmark)
        self.assertIn('QObject benchmarkCallbacks;',benchmark)
        self.assertIn('drawn_source_fps',benchmark)
    def test_draw_counter_is_not_a_paint_counter(self):
        s=(ROOT/'src/ui/viewer/TlViewport.cpp').read_text(encoding='utf-8')
        self.assertIn('time != _p->lastDrawnSourceTime',s)
        self.assertIn('std::isfinite(time)',s)
    def test_windows_are_destroyed_before_application_view_state(self):
        s=(ROOT/'src/ui/app/ApplicationRuntime.cpp').read_text(encoding='utf-8')
        shutdown=s[s.index('Application::~Application()'):s.index('bool Application::isAutomationMode()')]
        self.assertLess(shutdown.index('_pluginManager->shutdownPlugins()'),
                        shutdown.index('_mainWindow.reset()'))
        self.assertLess(shutdown.index('_annotationFallbackOverlayProvider.reset()'),
                        shutdown.index('_mainWindow.reset()'))
        self.assertIn('_secondaryWindows.clear()',shutdown)
if __name__=='__main__': unittest.main()
