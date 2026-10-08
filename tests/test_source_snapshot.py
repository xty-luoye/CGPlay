import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('snapshot_validator', ROOT / 'tools/validate_source_snapshot.py')
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class SourceDistributionTests(unittest.TestCase):
    def setUp(self):
        artifacts = ROOT / 'tests/artifacts/source_snapshot'
        artifacts.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=artifacts)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'src').mkdir()
        (self.root / 'resources').mkdir()
        (self.root / 'CMakeLists.txt').write_text('project(Example)\n', encoding='utf-8')
        (self.root / 'LICENSE.txt').write_text('fixture\n', encoding='utf-8')
        (self.root / 'src/CMakeLists.txt').write_text('set(SOURCES\n main.cpp\n)\n', encoding='utf-8')
        (self.root / 'src/main.cpp').write_text('int main() {}\n', encoding='utf-8')
        (self.root / 'resources/resources.qrc').write_text(
            '<RCC><qresource><file>icon.svg</file></qresource></RCC>', encoding='utf-8')
        (self.root / 'resources/icon.svg').write_text('<svg/>', encoding='utf-8')

    def test_complete_fixture(self):
        result = VALIDATOR.validate(self.root)
        self.assertTrue(result['passed'], result)
        self.assertEqual(2, result['localSourceReferences'])

    def test_missing_resource_is_rejected(self):
        (self.root / 'resources/icon.svg').unlink()
        result = VALIDATOR.validate(self.root)
        self.assertFalse(result['passed'])
        self.assertTrue(any('icon.svg' in error for error in result['errors']))

    def test_missing_application_source_is_rejected(self):
        (self.root / 'src/main.cpp').unlink()
        result = VALIDATOR.validate(self.root)
        self.assertFalse(result['passed'])
        self.assertTrue(any('main.cpp' in error for error in result['errors']))

    def test_private_file_is_rejected(self):
        (self.root / 'auth.json').write_text('{}', encoding='utf-8')
        result = VALIDATOR.validate(self.root)
        self.assertFalse(result['passed'])
        self.assertTrue(any('auth.json' in error for error in result['errors']))


if __name__ == '__main__':
    unittest.main()
