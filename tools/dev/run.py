#!/usr/bin/env python3
"""run a snippet against a persistent exploration session: [SDIR=dev] run.py SNIPPET.py  (g = Game('runs/SDIR'))"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "py"))
from story import *
g = Game("runs/" + os.environ.get("SDIR", "dev"), strict=bool(os.environ.get("STRICT")))
exec(open(sys.argv[1]).read())
