"""Tests for release versions, metadata, header stamping, and the preparation CLI."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from scripts.prepare_release import (
    normalize_version,
    release_metadata,
    release_notes,
    stamp_version_header,
)

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / "version.h").read_text(encoding="utf-8")


class ReleaseVersionTests(unittest.TestCase):
    def test_supported_versions(self):
        for raw, expected in (
            ("1.0.5.2", "1.0.5.2"),
            ("v1.0.5.2", "1.0.5.2"),
            ("  v1.0.6  ", "1.0.6"),
            ("0.0.0", "0.0.0"),
            ("65535.65535.65535.65535", "65535.65535.65535.65535"),
        ):
            with self.subTest(raw=raw):
                self.assertEqual(normalize_version(raw), expected)

    def test_invalid_versions(self):
        for raw in (
            "", "1", "1.2", "1.2.3.4.5", "1.2.3-beta", "1.2.3+build", "vv1.2.3",
            "1.-2.3", "1.2.3/evil", "01.2.3", "1.02.3", "1.2.03", "1.2.3.04",
            "65536.0.0", "1.65536.0", "1.0.65536", "1.0.0.65536", "999999.0.0",
            "１.2.3", "1.2.3\nother=value", '1.2.3"; Write-Error injected',
        ):
            with self.subTest(raw=raw):
                with self.assertRaises(ValueError):
                    normalize_version(raw)

    def test_metadata_matches_existing_releases(self):
        self.assertEqual(release_metadata("v1.0.5.1"), {
            "version": "1.0.5.1",
            "tag": "v1.0.5.1",
            "title": "Diablo 4 Rotation Spam v1.0.5.1",
            "archive": "d4rt_v1051.zip",
        })
        self.assertEqual(release_metadata("1.0.6")["archive"], "d4rt_v106.zip")

    def test_all_six_version_definitions_are_updated(self):
        stamped = stamp_version_header(HEADER, "2.3.4.5")
        for definition in (
            "#define APP_VER_MAJOR 2", "#define APP_VER_MINOR 3",
            "#define APP_VER_PATCH 4", "#define APP_VER_BUILD 5",
            '#define APP_VERSION_STR "2.3.4.5"', '#define APP_VERSION_STR_W L"2.3.4.5"',
        ):
            self.assertIn(definition + "\n", stamped)
        for line in HEADER.splitlines():
            if not line.startswith(("#define APP_VER_", "#define APP_VERSION_STR")):
                self.assertIn(line, stamped)
        self.assertEqual(stamp_version_header(stamped, "v2.3.4.5"), stamped)

    def test_three_part_version_has_zero_build_component(self):
        stamped = stamp_version_header(HEADER, "1.2.3")
        self.assertIn("#define APP_VER_BUILD 0\n", stamped)
        self.assertIn('#define APP_VERSION_STR "1.2.3"\n', stamped)
        self.assertIn('#define APP_VERSION_STR_W L"1.2.3"\n', stamped)

    def test_missing_or_duplicate_definition_is_an_error(self):
        for header in (
            HEADER.replace("#define APP_VER_BUILD", "#define RENAMED_BUILD"),
            HEADER + "\n#define APP_VER_BUILD 2\n",
        ):
            with self.subTest(header=header):
                with self.assertRaises(ValueError):
                    stamp_version_header(header, "1.2.3.4")

    def test_bilingual_notes_use_actual_archive_name(self):
        notes = release_notes("d4rt_v1052.zip")
        self.assertIn("### Версия для Windows", notes)
        self.assertIn("### Release for Windows", notes)
        self.assertEqual(notes.count("**d4rt_v1052.zip**"), 2)
        self.assertEqual(notes.count("**d4rt.exe**"), 2)
        self.assertNotIn(".rar", notes)

    def test_cli_writes_header_notes_and_github_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "scripts").mkdir()
            script = root / "scripts" / "prepare_release.py"
            shutil.copyfile(ROOT / "scripts" / script.name, script)
            header = root / "version.h"
            header.write_text(HEADER, encoding="utf-8")
            output = root / "github-output.txt"
            output.write_text("previous=value\n", encoding="utf-8")
            env = {**os.environ, "GITHUB_OUTPUT": str(output)}

            invalid = subprocess.run(
                [sys.executable, str(script), "1.0.5.2\nextra=value"],
                env=env, capture_output=True, text=True,
            )
            self.assertNotEqual(invalid.returncode, 0)
            self.assertEqual(header.read_text(encoding="utf-8"), HEADER)
            self.assertFalse((root / "out").exists())
            self.assertEqual(output.read_text(encoding="utf-8"), "previous=value\n")

            valid = subprocess.run(
                [sys.executable, str(script), "v1.0.5.2"],
                env=env, capture_output=True, text=True,
            )
            self.assertEqual(valid.returncode, 0, valid.stderr)
            self.assertEqual(header.read_text(encoding="utf-8"), stamp_version_header(HEADER, "1.0.5.2"))
            self.assertEqual(
                (root / "out" / "release-notes.md").read_text(encoding="utf-8"),
                release_notes("d4rt_v1052.zip"),
            )
            self.assertEqual(output.read_text(encoding="utf-8"),
                "previous=value\nversion=1.0.5.2\ntag=v1.0.5.2\n"
                "title=Diablo 4 Rotation Spam v1.0.5.2\narchive=d4rt_v1052.zip\n")


if __name__ == "__main__":
    unittest.main()
