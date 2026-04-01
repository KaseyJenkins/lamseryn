#!/usr/bin/env python3

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Tuple


@dataclass(frozen=True)
class Profile:
    name: str
    values: Dict[str, int]


def _parse_int(value: str) -> int:
    value = value.strip()
    # allow basic KiB/MiB suffixes used in docs (optional)
    multipliers = {
        "K": 1000,
        "KB": 1000,
        "M": 1000 * 1000,
        "MB": 1000 * 1000,
        "KiB": 1024,
        "MiB": 1024 * 1024,
    }
    for suf, mult in multipliers.items():
        if value.endswith(suf):
            return int(value[: -len(suf)].strip()) * mult
    return int(value)


def load_profiles(path: Path) -> Dict[str, Profile]:
    """Load a very small YAML subset.

    Supported format:
      profile-name:
        KEY: 123
        OTHER: 456

    Comments (# ...) and blank lines are ignored.
    """

    text = path.read_text(encoding="utf-8").splitlines()
    current_name = None
    current: Dict[str, int] = {}
    out: Dict[str, Profile] = {}

    def flush() -> None:
        nonlocal current_name, current
        if current_name is not None:
            out[current_name] = Profile(name=current_name, values=dict(current))
        current_name = None
        current = {}

    for raw in text:
        line = raw.split("#", 1)[0].rstrip("\n")
        if not line.strip():
            continue

        if not line.startswith(" ") and line.endswith(":"):
            flush()
            current_name = line[:-1].strip()
            continue

        if current_name is None:
            raise ValueError(f"Invalid profiles file; key outside profile: {raw}")

        if not line.startswith("  "):
            raise ValueError(f"Invalid indent (expected 2 spaces): {raw}")

        kv = line.strip()
        if ":" not in kv:
            raise ValueError(f"Invalid key/value line: {raw}")
        k, v = kv.split(":", 1)
        k = k.strip()
        v = v.strip()
        if not k:
            raise ValueError(f"Empty key: {raw}")
        if not v:
            continue
        current[k] = _parse_int(v)

    flush()
    return out


def get_profile(path: Path, name: str) -> Profile:
    profiles = load_profiles(path)
    if name not in profiles:
        known = ", ".join(sorted(profiles.keys()))
        raise SystemExit(f"Unknown profile '{name}'. Known: {known}")
    return profiles[name]


def defs_for_make(profile: Profile, *, allowlist: Tuple[str, ...]) -> str:
    """Return a string of -D... flags suitable for Makefile APP_DEFS."""
    parts = []
    for key in allowlist:
        if key in profile.values:
            parts.append(f"-D{key}={profile.values[key]}")
    return " ".join(parts)
