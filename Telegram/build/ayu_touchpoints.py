#!/usr/bin/env python3
import re
import subprocess
import sys

ROOT = 'Telegram/SourceFiles'
PATTERN = re.compile(r'\bAyu|\bayu[_A-Z]|"ayu/|\bFresh[A-Z]|\bfresh[_A-Z]')


def tracked_sources():
    out = subprocess.check_output(['git', 'ls-files', ROOT], text=True)
    for path in out.splitlines():
        if path.startswith(ROOT + '/ayu/'):
            continue
        if path.endswith(('.cpp', '.h', '.mm', '.style')):
            yield path


def main():
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    rows = []
    for path in tracked_sources():
        with open(path, encoding='utf-8', errors='replace') as f:
            count = sum(1 for line in f if PATTERN.search(line))
        if count:
            rows.append((count, path))
    rows.sort(reverse=True)
    total = sum(count for count, _ in rows)
    for count, path in rows[:limit or None]:
        print(f'{count:5d}  {path}')
    print(f'{len(rows)} files, {total} lines outside {ROOT}/ayu/')


if __name__ == '__main__':
    main()
