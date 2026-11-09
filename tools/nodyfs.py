#!/usr/bin/env python3
"""Inspect the NodyFS volume inside nody.img from the host.

    tools/nodyfs.py [image]                 list files
    tools/nodyfs.py [image] cat NAME        hex dump a file
    tools/nodyfs.py [image] get NAME OUT    copy a file out of the image
    tools/nodyfs.py [image] scene NAME      decode a .flw scene
"""
import struct
import sys

FS_LBA, BLOCK = 2048, 4096


def volume(path):
    img = open(path, "rb").read()
    sb = img[FS_LBA * 512:FS_LBA * 512 + 512]
    if sb[:8] != b"NODYFS01":
        raise SystemExit("no NodyFS volume found (boot the image once to create it)")
    _, total, dir_lba, dir_sec, fat_lba, fat_sec, data_lba, _ = struct.unpack("<8I", sb[8:40])
    fat = struct.unpack("<%dI" % (fat_sec * 128), img[fat_lba * 512:(fat_lba + fat_sec) * 512])
    files = {}
    for i in range(dir_sec * 512 // 64):
        e = img[dir_lba * 512 + i * 64:dir_lba * 512 + (i + 1) * 64]
        size, first, flags, mtime = struct.unpack("<4I", e[32:48])
        if not flags & 1:
            continue
        data, b = b"", first
        while b not in (0, 0xFFFFFFFF) and len(data) < size:
            data += img[data_lba * 512 + b * BLOCK:data_lba * 512 + (b + 1) * BLOCK]
            b = fat[b]
        when = "%04d-%02d-%02d %02d:%02d" % (2000 + (mtime >> 26), (mtime >> 22) & 15, (mtime >> 17) & 31,
                                           (mtime >> 12) & 31, (mtime >> 6) & 63)
        files[e[:32].split(b"\0")[0].decode()] = (data[:size], when)
    free = sum(1 for b in fat[1:total] if b == 0) * BLOCK
    return files, free


def scene(data):
    if data[:4] != b"FLOW":
        raise SystemExit("not a Flow scene")
    ver, count, display, selected, flags, loop = struct.unpack("<HHHHII", data[4:20])
    cam = struct.unpack("<6f", data[20:44])
    print(f"Flow scene v{ver}: {count} nodes, display node {display}, loop {loop} frames, flags {flags:#x}")
    print("camera yaw %.2f pitch %.2f distance %.2f target (%.2f, %.2f, %.2f)" % cam)
    o = 56
    for i in range(count):
        name = data[o:o + 12].split(b"\0")[0].decode()
        x, y, fl, np_, nin = struct.unpack("<hhBBB", data[o + 12:o + 19])
        params = struct.unpack("<%df" % np_, data[o + 20:o + 20 + np_ * 4])
        ins = list(data[o + 20 + np_ * 4:o + 20 + np_ * 4 + nin])
        o += 20 + np_ * 4 + ((nin + 3) & ~3)
        wires = ", ".join("-" if s == 255 else f"#{s}" for s in ins)
        print(f"  #{i:<2} {name:<10} at ({x:4},{y:4}){' bypass' if fl & 1 else ''}  inputs [{wires}]")
        print("      " + " ".join("%g" % round(p, 4) for p in params))


if __name__ == "__main__":
    args = sys.argv[1:]
    image = args.pop(0) if args and args[0].endswith(".img") else "nody.img"
    files, free = volume(image)
    if not args:
        for name in sorted(files):
            print("%-28s %7d bytes   %s" % (name, len(files[name][0]), files[name][1]))
        print("%d files, %d KB free" % (len(files), free // 1024))
    elif args[0] in ("cat", "get", "scene") and len(args) > 1 and args[1] in files:
        data = files[args[1]][0]
        if args[0] == "scene":
            scene(data)
        elif args[0] == "get":
            open(args[2], "wb").write(data)
        else:
            for i in range(0, len(data), 16):
                row = data[i:i + 16]
                print("%04x  %-48s %s" % (i, row.hex(" "), "".join(chr(c) if 32 <= c < 127 else "." for c in row)))
    else:
        raise SystemExit(__doc__)
