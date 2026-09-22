"""Every UI string must have English/Russian resources and a loader mapping."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent


class LocalizationTests(unittest.TestCase):
    def test_all_strings_are_localized_and_loaded(self):
        header = (ROOT / "app_state.h").read_text(encoding="utf-8")
        block = header.split("struct LocStrings {", 1)[1].split("};", 1)[0]
        fields = set(re.findall(r"std::string\s+(\w+)\s*=", block))
        loader = (ROOT / "macro.cpp").read_text(encoding="utf-8")
        for language in ("en", "ru"):
            lines = (ROOT / f"lang_{language}.txt").read_text(encoding="utf-8-sig").splitlines()
            pairs = [line.split("=", 1) for line in lines if line and not line.startswith("#")]
            resources = dict(pairs)
            self.assertEqual(len(resources), len(pairs), "Duplicate translation keys")
            self.assertEqual(set(resources), fields)
            for key, value in resources.items():
                with self.subTest(language=language, key=key):
                    self.assertTrue(value.strip())
                    self.assertRegex(loader, rf'key == "{key}"\)\s*lang\.{key} = val;')

    def test_events_hotkey_labels(self):
        text = (ROOT / "lang_ru.txt").read_text(encoding="utf-8")
        self.assertIn("\nevents=Эвенты:\n", text)
        self.assertIn("\nbtnEvents=Открыть эвенты:\n", text)

    def test_event_files_are_registered_in_visual_studio(self):
        ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
        sources = {"event_schedule.cpp", "event_service.cpp", "event_log.cpp"}
        headers = {"event_schedule.h", "event_service.h", "event_log.h",
                   "third_party\\picojson\\picojson.h"}
        for name in ("diablo 4.vcxproj", "diablo 4.vcxproj.filters"):
            project = ET.parse(ROOT / name)
            compiled = {item.attrib["Include"] for item in project.findall(".//m:ClCompile[@Include]", ns)}
            included = {item.attrib["Include"] for item in project.findall(".//m:ClInclude[@Include]", ns)}
            self.assertTrue(sources <= compiled, name)
            self.assertTrue(headers <= included, name)
        for path in sources | headers:
            self.assertTrue((ROOT / path.replace("\\", "/")).is_file(), path)


if __name__ == "__main__":
    unittest.main()
