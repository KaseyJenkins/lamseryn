#!/usr/bin/env python3

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict


_KV_RE = re.compile(r"(?P<k>[a-zA-Z0-9_]+)=(?P<v>[-+]?[0-9]+)")


@dataclass
class ParsedCounters:
    # aggregated across threads
    totals: Dict[str, int]


def parse_thread_counters(log_text: str) -> ParsedCounters:
    """Parse key=value counters from the server's thread exit logs.

    This is intentionally tolerant: we scan all key=value pairs on lines that
    look like 'Thread N:' and sum across threads.
    """

    totals: Dict[str, int] = {}
    for line in log_text.splitlines():
        if "Thread " not in line:
            continue
        # Heuristic: only parse lines that have at least one key=value
        if "=" not in line:
            continue
        for m in _KV_RE.finditer(line):
            k = m.group("k")
            v = int(m.group("v"))
            totals[k] = totals.get(k, 0) + v
    return ParsedCounters(totals=totals)


def get_int(c: ParsedCounters, key: str, default: int = 0) -> int:
    return int(c.totals.get(key, default))
