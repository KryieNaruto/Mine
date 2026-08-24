#!/usr/bin/env python3
"""薄封装:转发到 tools/fetch-deps.py,固定 --project 指向本项目。"""
import os
import shlex
import sys

_here = os.path.dirname(os.path.abspath(__file__))   # <proj>/scripts
_proj = os.path.dirname(_here)                       # <proj>
_root = os.path.dirname(_proj)                       # Mine
_tool = os.path.join(_root, "tools", "fetch-deps.py")
_args = [sys.executable, _tool, "--project", _proj] + sys.argv[1:]
sys.exit(os.system(" ".join(shlex.quote(a) for a in _args)))
