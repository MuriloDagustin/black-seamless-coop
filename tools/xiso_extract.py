"""Extract an Xbox XISO (XDVDFS) image: python -I xiso_extract.py <image.iso> <out_dir>"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# where the game partition starts: trimmed xiso, then full redump layouts
PARTITION_OFFSETS = (0, 0x18300000, 0xFD90000, 0x2080000)
ATTR_DIRECTORY = 0x10


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        head = f.read(28)
        if head[:20] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", head, 20)
            return base, root_sector, root_size
    raise SystemExit("no XDVDFS volume descriptor found")


def safe_name(name):
    if not name or name in (".", "..") or any(c in name for c in '/\\:*?"<>|') or "\0" in name:
        raise SystemExit(f"refusing unsafe file name {name!r}")
    return name


def copy_file(f, base, sector, size, path):
    f.seek(base + sector * SECTOR)
    with open(path, "wb") as out:
        left = size
        while left:
            chunk = f.read(min(left, 1 << 20))
            if not chunk:
                raise SystemExit(f"image ends inside {path}")
            out.write(chunk)
            left -= len(chunk)


def walk(f, base, dir_sector, dir_size, out_dir, stats, depth=0):
    if depth > 64:
        raise SystemExit("directory tree too deep")
    if dir_size == 0:
        return
    f.seek(base + dir_sector * SECTOR)
    table = f.read(dir_size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, sector, size, attr, name_len = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # padding / empty directory
        name = safe_name(table[off + 14:off + 14 + name_len].decode("latin-1"))
        path = os.path.join(out_dir, name)
        if attr & ATTR_DIRECTORY:
            os.makedirs(path, exist_ok=True)
            walk(f, base, sector, size, path, stats, depth + 1)
            stats["dirs"] += 1
        else:
            copy_file(f, base, sector, size, path)
            stats["files"] += 1
            stats["bytes"] += size
        if left and left != 0xFFFF:
            stack.append(left * 4)
        if right and right != 0xFFFF:
            stack.append(right * 4)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    image, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    stats = {"files": 0, "dirs": 0, "bytes": 0}
    with open(image, "rb") as f:
        base, root_sector, root_size = find_partition(f)
        print(f"partition at {base:#x}, root sector {root_sector:#x}, {root_size} bytes")
        walk(f, base, root_sector, root_size, out_dir, stats)
    print(f"{stats['files']} files, {stats['dirs']} directories, {stats['bytes'] / 2**20:.1f} MiB")


if __name__ == "__main__":
    main()
