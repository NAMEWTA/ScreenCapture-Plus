#!/usr/bin/env python3
"""Retain exact Ubuntu build records for already verified original runtime .debs.

This reads the successful collect-system-source-materials.py report. It queries
only official Launchpad, retains original API/.changes/.buildinfo/log bytes and
binds SHA256 records to the retained .deb. It neither installs packages nor
creates a NativeBuild receipt. Missing signatures/build inputs remain gaps.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from urllib.error import URLError

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('launchpad_build_records', ROOT / 'build/package/launchpad_build_records.py')
records = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system-sources', type=Path, required=True,
                        help='Successful original system-sources.json with retained .deb and Packages records')
    parser.add_argument('--series', required=True, help='Exact Ubuntu series name of the original runner, e.g. noble')
    parser.add_argument('--output', type=Path, required=True, help='New separate original-record directory')
    options = parser.parse_args()
    try:
        report = records.collect(options.system_sources, options.series, options.output, ROOT)
        print(json.dumps({key: report[key] for key in ('status', 'gaps')}, ensure_ascii=False, indent=2))
        return 0
    except (ValueError, KeyError, TypeError, OSError, URLError, subprocess.SubprocessError) as error:
        print('Original Launchpad build collection failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
