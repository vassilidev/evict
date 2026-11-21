#!/usr/bin/env python3
# Minifies the page shipped in the app: CSS and script through esbuild, whitespace between tags removed.
# The data placeholder script stays byte for byte, since the app injects the scan there.
# Usage: minify.py index.html out.html (needs Node for npx; build.sh falls back to the readable page)
import re
import subprocess
import sys

ESBUILD = ['npx', '--yes', 'esbuild@0.24.0', '--minify', '--target=safari16']
PLACEHOLDER = 'window.DATA = null; /* __DATA__ */'


def esbuild(code, loader):
    return subprocess.run(ESBUILD + ['--loader=' + loader], input=code, capture_output=True, text=True, check=True).stdout.strip()


def minify(part):
    if part.startswith('<style>'):
        return '<style>' + esbuild(part[7:-8], 'css') + '</style>'
    if part.startswith('<script>'):
        return part if PLACEHOLDER in part else '<script>' + esbuild(part[8:-9], 'js') + '</script>'
    return re.sub(r'>\s+<', '><', part)


src, dst = sys.argv[1:]
parts = re.split(r'(<style>.*?</style>|<script>.*?</script>)', open(src).read(), flags=re.S)
html = ''.join(minify(p) for p in parts)
assert PLACEHOLDER in html, 'data placeholder lost'
open(dst, 'w').write(html)
