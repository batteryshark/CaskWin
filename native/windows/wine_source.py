"""Read-only Wine source selection for CaskWin's Python inspection helpers.

Matches wine-source.ps1: explicit argument, CASKWIN_WINE_SOURCE, then the
standalone repository's prepared build/wine-source tree. Never prepares sources.
"""
import json
import os
import re
from pathlib import Path


def resolve_wine_source(directory=None):
    selected = directory or os.environ.get("CASKWIN_WINE_SOURCE")
    if not selected:
        selected = Path(__file__).resolve().parents[2] / "build" / "wine-source"
    source = Path(selected).expanduser().resolve()
    if (not (source / "configure").is_file() or not (source / "include").is_dir()
            or not (source / "server/unicode.c").is_file()
            or not (source / ".caskwin-source.json").is_file()):
        raise ValueError(
            f"Prepared Wine source tree not found at {source}; run "
            "scripts/prepare-wine.ps1 or supply --wine-source-directory "
            "for an existing prepared Wine tree."
        )
    marker = json.loads((source / ".caskwin-source.json").read_text(encoding="utf-8-sig"))
    if (marker.get("schemaVersion") != 1
            or any(not isinstance(marker.get(key), str)
                   or not re.fullmatch(r"[0-9a-fA-F]{%d}" % size, marker[key])
                   for key, size in (("baseCommit", 40), ("patchedTree", 40),
                                     ("profileSha256", 64)))):
        raise ValueError(f"Unsupported prepared Wine source marker at {source}; prepare the profile again.")
    return source
