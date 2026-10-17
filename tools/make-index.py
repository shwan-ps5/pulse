#!/usr/bin/env python3
# Writes index.txt in a songs folder: one "relative/path.mp3<TAB>size" line per MP3, found recursively.
# The title cannot always list folders on the console, so it reads this file instead.
import os, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
lines = []
for d, _, files in os.walk(root):
    for f in files:
        if f.lower().endswith(".mp3"):
            p = os.path.join(d, f)
            lines.append("%s\t%d" % (os.path.relpath(p, root).replace(os.sep, "/"), os.path.getsize(p)))
with open(os.path.join(root, "index.txt"), "w", encoding="utf-8", newline="\n") as out:
    out.write("\n".join(sorted(lines)) + ("\n" if lines else ""))
print(len(lines), "songs indexed in", os.path.join(root, "index.txt"))
