#!/usr/bin/env python3
"""Resolve the next immutable C5VRX-4 semantic alpha version from git tags."""
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
PATTERN = re.compile(r'^c5vrx4-v(4)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-alpha\.([1-9][0-9]*)$')

def resolve_version(tags, same_commit_tags=()):
    def parsed(tag):
        match = PATTERN.fullmatch(tag)
        return tuple(map(int, match.groups())) if match else None
    existing = [(parsed(tag), tag) for tag in same_commit_tags if parsed(tag)]
    if existing:
        tag = max(existing)[1]
        return {'version': tag.removeprefix('c5vrx4-v'), 'tag': tag}
    versions = [parsed(tag) for tag in tags if parsed(tag)]
    major, minor, patch, alpha = max(versions, default=(4, 0, 0, 0))
    version = f'{major}.{minor}.{patch}-alpha.{alpha + 1}'
    return {'version': version, 'tag': f'c5vrx4-v{version}'}

def main():
    command = ['git', '-c', f'safe.directory={ROOT}', 'tag']
    tags = subprocess.check_output(command + ['--list'], cwd=ROOT, text=True).splitlines()
    same = subprocess.check_output(command + ['--points-at', os.environ['GITHUB_SHA']], cwd=ROOT, text=True).splitlines()
    print(json.dumps(resolve_version(tags, same)))

if __name__ == '__main__':
    main()
