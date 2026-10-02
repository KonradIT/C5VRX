#!/usr/bin/env python3
"""Fingerprint tracked firmware inputs, excluding website and documentation."""
import hashlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def firmware_input(path, generation):
    if path.endswith('.md'):
        return False
    return (path.startswith(('main/', 'managed_components/'))
            or path in ('CMakeLists.txt', 'partitions.csv', 'dependencies.lock',
                        'tools/firmware_input_hash.py')
            or path.startswith('sdkconfig')
            or path == f'.github/workflows/{"build" if generation == "3" else "c5vrx4"}.yml'
            or (generation == '4' and (path.startswith('experiments/c5vrx-4/')
                                      or path == 'tools/c5vrx4_version.py')))

def main():
    generation = sys.argv[1]
    if generation not in ('3', '4'):
        raise SystemExit('Expected generation 3 or 4')
    paths = subprocess.check_output(
        ['git', '-c', f'safe.directory={ROOT}', 'ls-files', '-z'], cwd=ROOT
    ).decode().split('\0')
    digest = hashlib.sha256()
    for path in sorted(p for p in paths if firmware_input(p, generation)):
        digest.update(path.encode() + b'\0')
        digest.update(hashlib.sha256((ROOT / path).read_bytes()).digest())
    print(digest.hexdigest())

if __name__ == '__main__':
    main()
