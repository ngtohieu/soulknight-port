#!/usr/bin/env python3
"""Join Soul Knight's split asset files back into whole files.

The Android build stores large serialized files as name.split0, name.split1,
... so that each APK entry stays under 1 MiB. Unity on Android reassembles
them through its APK reader; on disk the so-loader is safer with the plain
file, so every split set is concatenated in order and the parts are removed.
"""

import os
import re
import sys

DATA = os.path.join("assets", "bin", "Data")
SPLIT_RE = re.compile(r"^(.+)\.split(\d+)$")
CHUNK = 1 << 20


def fail(message):
    print("merge-splits: %s" % message, file=sys.stderr, flush=True)
    raise SystemExit(1)


def progress(done, total, detail):
    print("NXEXTRACT_PROGRESS %d %d %s" % (done, total, detail), flush=True)


def main():
    if len(sys.argv) != 2:
        fail("usage: merge-splits.py <stage>")
    data_dir = os.path.join(sys.argv[1], DATA)
    if not os.path.isdir(data_dir):
        fail("missing %s" % data_dir)

    groups = {}
    for root, _dirs, files in os.walk(data_dir):
        for name in files:
            match = SPLIT_RE.match(name)
            if match:
                key = os.path.join(root, match.group(1))
                groups.setdefault(key, []).append((int(match.group(2)), name))

    total = len(groups)
    for done, (target, parts) in enumerate(sorted(groups.items()), 1):
        parts.sort()
        indexes = [index for index, _name in parts]
        if indexes != list(range(len(parts))):
            fail("%s: split parts are not contiguous: %s" % (target, indexes))
        root = os.path.dirname(target)
        temp = target + ".merging"
        with open(temp, "wb") as out:
            for _index, name in parts:
                with open(os.path.join(root, name), "rb") as part:
                    while True:
                        block = part.read(CHUNK)
                        if not block:
                            break
                        out.write(block)
        os.replace(temp, target)
        for _index, name in parts:
            os.remove(os.path.join(root, name))
        progress(done, total, "joining %s" % os.path.basename(target))

    print("merge-splits: joined %d file(s)" % total, flush=True)


if __name__ == "__main__":
    main()
