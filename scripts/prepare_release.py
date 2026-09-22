"""Stamp the build version and prepare metadata for the manual release workflow."""

import argparse
import os
from pathlib import Path
import re


def normalize_version(value: str) -> str:
    match = re.fullmatch(r"v?([0-9]{1,5}(?:\.[0-9]{1,5}){2,3})", value.strip())
    if match is None:
        raise ValueError("Use a version like 1.0.5 or 1.0.5.2 (optional v prefix).")
    version = match.group(1)
    parts = version.split(".")
    if any(len(part) > 1 and part.startswith("0") for part in parts):
        raise ValueError("Version components must not have leading zeros.")
    if any(int(part) > 65535 for part in parts):
        raise ValueError("Each Windows version component must be between 0 and 65535.")
    return version


def release_metadata(value: str) -> dict[str, str]:
    version = normalize_version(value)
    return {
        "version": version,
        "tag": f"v{version}",
        "title": f"Diablo 4 Rotation Spam v{version}",
        "archive": f"d4rt_v{version.replace('.', '')}.zip",
    }


def stamp_version_header(header: str, value: str) -> str:
    version = normalize_version(value)
    parts = version.split(".")
    parts += ["0"] * (4 - len(parts))
    replacements = dict(zip(
        ("APP_VER_MAJOR", "APP_VER_MINOR", "APP_VER_PATCH", "APP_VER_BUILD"), parts
    ))
    replacements["APP_VERSION_STR"] = f'"{version}"'
    replacements["APP_VERSION_STR_W"] = f'L"{version}"'
    for name, replacement in replacements.items():
        pattern = rf"^([ \t]*#define[ \t]+{name}[ \t]+)[^\r\n]*"
        header, count = re.subn(
            pattern, lambda match: match.group(1) + replacement, header,
            flags=re.MULTILINE,
        )
        if count != 1:
            raise ValueError(f"Expected exactly one definition of {name}, found {count}.")
    return header


def release_notes(archive: str) -> str:
    return f"""### Версия для Windows

1. Скачайте **{archive}**
2. Распакуйте архив
3. Запустите **d4rt.exe**

### Release for Windows

1. Download **{archive}**
2. Extract files
3. Run **d4rt.exe**
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="Release version, e.g. 1.0.5.2")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    header_path = root / "src" / "version.h"
    try:
        metadata = release_metadata(args.version)
        header = stamp_version_header(header_path.read_text(encoding="utf-8"), metadata["version"])
    except ValueError as error:
        parser.error(str(error))

    # Build-workspace changes only: the workflow never commits or pushes a branch.
    header_path.write_text(header, encoding="utf-8", newline="\n")
    output_dir = root / "out"
    output_dir.mkdir(exist_ok=True)
    (output_dir / "release-notes.md").write_text(
        release_notes(metadata["archive"]), encoding="utf-8", newline="\n"
    )
    if output_path := os.environ.get("GITHUB_OUTPUT"):
        with open(output_path, "a", encoding="utf-8", newline="\n") as output:
            for name, value in metadata.items():
                output.write(f"{name}={value}\n")
    print(f"Prepared {metadata['tag']}: {metadata['archive']}")


if __name__ == "__main__":
    main()
