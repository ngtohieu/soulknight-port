#!/usr/bin/env python3
"""Store Soul Knight's AssetBundles uncompressed.

Every bundle in the APK is one LZMA block (109 MiB on disk, 800 MiB
unpacked; "common" alone is 178 MiB). Unity cannot seek inside LZMA, so it
inflates the whole bundle into RAM on a single core each time it is loaded:
the stall before HeroRoom and the out-of-memory freeze on 1 GB handhelds.
Android hides this by caching the result; the port has no such cache.

This rewrites each LZMA bundle as an uncompressed UnityFS archive (same
directory, same data, one uncompressed block) that Unity reads straight
from the card on demand. Already-converted or non-LZMA bundles are skipped,
so it is safe to run again. Runs as an nxextract hook on the stage and, for
installs made before it existed, once from port-env.sh on the game dir.
"""

import lzma
import os
import shutil
import struct
import sys

BUNDLES = os.path.join("assets", "AssetBundles")
MARKER = ".sk-unpacked-v1"
TEMP_SUFFIX = ".unpacking"
CHUNK = 1 << 20
# Leave this much free on the card after each converted bundle.
RESERVE = 64 << 20

FLAG_COMPRESSION = 0x3F
FLAG_DIRECTORY_COMBINED = 0x40
FLAG_INFO_AT_END = 0x80
FLAG_PADDING_AT_START = 0x200
BLOCK_STREAMED = 0x40


class Skip(Exception):
    pass


def log(message):
    print("unpack-bundles: %s" % message, flush=True)


def progress(done, total, detail):
    print("NXEXTRACT_PROGRESS %d %d %s" % (done, total, detail), flush=True)


def read_cstr(data, offset):
    end = data.index(b"\0", offset)
    return data[offset:end], end + 1


def parse(path):
    """Returns the header fields and block table, or raises Skip."""
    with open(path, "rb") as stream:
        head = stream.read(4096)
        if not head.startswith(b"UnityFS\0"):
            raise Skip("not UnityFS")
        offset = 8
        version = struct.unpack_from(">I", head, offset)[0]
        offset += 4
        player, offset = read_cstr(head, offset)
        engine, offset = read_cstr(head, offset)
        _size, info_csize, info_usize, flags = struct.unpack_from(">QIII", head, offset)
        offset += 20
        if version >= 7:
            offset = (offset + 15) & ~15
        if flags & FLAG_INFO_AT_END:
            stream.seek(-info_csize, os.SEEK_END)
            info_start = None
        else:
            stream.seek(offset)
            info_start = offset
        info = stream.read(info_csize)

    method = flags & FLAG_COMPRESSION
    if method == 0:
        pass
    elif method == 1:
        info = lzma_decode(info, info_usize)
    elif method in (2, 3):
        info = lz4_block_decode(info, info_usize)
    else:
        raise Skip("block info compression %d" % method)
    if len(info) != info_usize:
        raise Skip("block info size mismatch")

    count = struct.unpack_from(">I", info, 16)[0]
    blocks = [struct.unpack_from(">IIH", info, 20 + index * 10) for index in range(count)]
    if not any(block_flags & FLAG_COMPRESSION == 1 for _u, _c, block_flags in blocks):
        raise Skip("no LZMA blocks")
    if any(block_flags & FLAG_COMPRESSION not in (0, 1) for _u, _c, block_flags in blocks):
        raise Skip("mixed LZ4 blocks")
    # The directory (node table) follows the block table unchanged.
    directory = info[20 + count * 10:]

    data_start = (info_start + info_csize) if info_start is not None else offset
    if flags & FLAG_PADDING_AT_START:
        data_start = (data_start + 15) & ~15
    return {
        "version": version,
        "player": player,
        "engine": engine,
        "flags": flags,
        "hash": info[:16],
        "blocks": blocks,
        "directory": directory,
        "data_start": data_start,
    }


def lzma_filter(props):
    if len(props) != 5:
        raise Skip("bad LZMA properties")
    value = props[0]
    lc, value = value % 9, value // 9
    lp, pb = value % 5, value // 5
    return [{
        "id": lzma.FILTER_LZMA1,
        "dict_size": struct.unpack("<I", props[1:5])[0],
        "lc": lc,
        "lp": lp,
        "pb": pb,
    }]


def lzma_decode(data, size):
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=lzma_filter(data[:5]))
    return decoder.decompress(data[5:], max_length=size)


def lz4_block_decode(data, size):
    # Only the small block-info table ever comes here (LZ4HC in this game).
    out = bytearray()
    pos = 0
    while pos < len(data):
        token = data[pos]
        pos += 1
        length = token >> 4
        if length == 15:
            while True:
                extra = data[pos]
                pos += 1
                length += extra
                if extra != 255:
                    break
        out += data[pos:pos + length]
        pos += length
        if pos >= len(data):
            break
        back = data[pos] | (data[pos + 1] << 8)
        pos += 2
        match = token & 15
        if match == 15:
            while True:
                extra = data[pos]
                pos += 1
                match += extra
                if extra != 255:
                    break
        match += 4
        start = len(out) - back
        if back <= 0 or start < 0:
            raise Skip("corrupt LZ4 block info")
        for index in range(match):
            out.append(out[start + index])
    if len(out) != size:
        raise Skip("LZ4 block info size mismatch")
    return bytes(out)


def write_uncompressed(path, info, temp):
    total = sum(usize for usize, _c, _f in info["blocks"])
    block_info = (
        info["hash"]
        + struct.pack(">I", 1)
        + struct.pack(">IIH", total, total, BLOCK_STREAMED)
        + info["directory"]
    )
    flags = FLAG_DIRECTORY_COMBINED
    header = (
        b"UnityFS\0"
        + struct.pack(">I", info["version"])
        + info["player"] + b"\0"
        + info["engine"] + b"\0"
    )
    fixed_len = len(header) + 20
    if info["version"] >= 7:
        fixed_len = (fixed_len + 15) & ~15
    file_size = fixed_len + len(block_info) + total
    header += struct.pack(">QIII", file_size, len(block_info), len(block_info), flags)
    header += b"\0" * (fixed_len - len(header))

    written = 0
    with open(path, "rb") as source, open(temp, "wb") as out:
        out.write(header)
        out.write(block_info)
        source.seek(info["data_start"])
        for usize, csize, block_flags in info["blocks"]:
            if block_flags & FLAG_COMPRESSION == 0:
                remaining = csize
                while remaining:
                    piece = source.read(min(CHUNK, remaining))
                    if not piece:
                        raise Skip("truncated block")
                    out.write(piece)
                    remaining -= len(piece)
                written += csize
                continue
            props = source.read(5)
            decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=lzma_filter(props))
            remaining_in = csize - 5
            produced = 0
            pending = b""
            while produced < usize:
                if decoder.needs_input:
                    if remaining_in <= 0:
                        break
                    pending = source.read(min(CHUNK, remaining_in))
                    if not pending:
                        break
                    remaining_in -= len(pending)
                else:
                    pending = b""
                piece = decoder.decompress(pending, max_length=min(CHUNK * 4, usize - produced))
                out.write(piece)
                produced += len(piece)
            if produced != usize:
                raise Skip("LZMA block decoded to %d of %d bytes" % (produced, usize))
            # Skip any unread tail of this block (end marker, padding).
            if remaining_in > 0:
                source.seek(remaining_in, os.SEEK_CUR)
            written += usize
        out.flush()
        os.fsync(out.fileno())
    if written != total or os.path.getsize(temp) != file_size:
        raise Skip("output size mismatch")
    return total


def main():
    if len(sys.argv) != 2:
        print("usage: unpack-bundles.py <dir containing assets/>", file=sys.stderr)
        raise SystemExit(2)
    root = os.path.join(sys.argv[1], BUNDLES)
    if not os.path.isdir(root):
        log("no %s, nothing to do" % root)
        return
    marker = os.path.join(root, MARKER)
    if os.path.exists(marker):
        return

    candidates = []
    for current, _dirs, files in os.walk(root):
        for name in sorted(files):
            path = os.path.join(current, name)
            if name.endswith(TEMP_SUFFIX):
                os.remove(path)
                continue
            if name.endswith(".manifest") or name == MARKER:
                continue
            candidates.append(path)
    candidates.sort()

    converted = skipped = 0
    grown = 0
    total = len(candidates)
    for done, path in enumerate(candidates, 1):
        relative = os.path.relpath(path, root)
        try:
            info = parse(path)
        except Skip:
            skipped += 1
            continue
        except (OSError, ValueError, struct.error, lzma.LZMAError) as error:
            log("%s: unreadable (%s), left as is" % (relative, error))
            skipped += 1
            continue
        need = sum(usize for usize, _c, _f in info["blocks"]) + RESERVE
        if shutil.disk_usage(root).free < need:
            log("not enough free space for %s (%d MiB needed); stopping, game still runs"
                % (relative, need >> 20))
            return
        temp = path + TEMP_SUFFIX
        before = os.path.getsize(path)
        try:
            write_uncompressed(path, info, temp)
            os.replace(temp, path)
        except (Skip, OSError, lzma.LZMAError) as error:
            if os.path.exists(temp):
                os.remove(temp)
            log("%s: %s, left as is" % (relative, error))
            skipped += 1
            continue
        converted += 1
        grown += os.path.getsize(path) - before
        progress(done, total, "unpacking %s" % relative)

    with open(marker, "w") as stream:
        stream.write("%d converted\n" % converted)
    log("converted %d bundle(s), skipped %d, +%d MiB on disk" % (converted, skipped, grown >> 20))


if __name__ == "__main__":
    main()
