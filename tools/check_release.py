#!/usr/bin/env python3
"""Release audit for th07-vita-native.

Checks a source tree (the git-tracked files when it is a git work tree) and/or
a VPK and exits non-zero on anything that must not be published:

  * original game files or files made from them: DAT archives, WAV/MIDI music,
    the EXE/ICO, replays, score/config files, MS Gothic, gm.dls, TDSB
    instrument files, SoundFonts; any icon whose pixels are the EXE's icon;
  * user data and run output: score.dat, *.rpy, *.cfg, log.txt, *.log,
    screenshots/raw frame dumps, anything under replay/ or snapshot(s)/;
  * binary files other than the allowed ones (compiled shaders, the bundled
    Noto Sans JP, the generated icon0.png, and eboot.bin/param.sfo in a VPK);
  * absolute build-machine paths, credentials, and the author's mail address.

Usage: python3 tools/check_release.py [--repo DIR] [--vpk FILE] [--elf build/th07] [--exe ORIGINAL.exe]
With --exe the original icon is extracted with vita/ExeIcon-equivalent rules
(via Pillow) and its pixels are added to the forbidden list.
"""
import argparse
import hashlib
import io
import os
import re
import subprocess
import sys
import zipfile

# Allowed binaries, pinned by SHA-256.
NOTO_SHA = "6ab1664d8adc20b19237ddc451c94e31f493cb851a1917242debf66f9af6da05"
ALLOWED_BINARY_SUFFIX = (".gxp",)
ALLOWED_BINARY_NAMES = {"NotoSansJP-Regular.ttf": NOTO_SHA}

# Known icons made from the original th07.exe (file hashes and RGBA hashes:
# extract_exe_icons.py indexed/RGBA PNGs, vita/ExeIcon.cpp PNG; 128x128 and 32x32 pixels).
EXE_ICON_FILE_SHA = {
    "9d9f8fd7c9953559a884c5665766e442eb8b312f38458cdf0b8447a31599286e",
    "55fc4bed3081e287059a9641c90c9c41831c1bf01a2e391d18d00c04ff4f41c2",
    "217541b1ff550dd525efc9c7b8326e27681dc345a59265aea451a79df4d6e02f",
}
EXE_ICON_RGBA_SHA = {
    "a57a4f2d90f22a54f2d89bdf7a160a3ac875984bcf51d5ffece65ceaeb17c490",  # 128x128
    "683724586a621d134fd3ec5adaf9acad1e99eb94b9132ed6ef6c0803d55056cf",  # 32x32
}

FORBIDDEN_NAME = re.compile(
    r"(\.(dat|wav|pos|mid|midi|rmi|exe|dll|ico|rpy|cfg|ttc|dls|tdsb|sf2|bmp|rgba|raw|tga|log|pbp)$)"
    r"|(^|/)(log\.txt|score\.dat|msgothic[^/]*|gm\.dls)$"
    r"|(^|/)(replay|replays|snapshot|snapshots|shots|screenshots|run-logs|runs|device-backups)/"
    r"|(^|/)bgm/",
    re.IGNORECASE)
# Files that look like build configuration are not user config; none are needed today.
CFG_WHITELIST = set()

# Patterns assembled at run time so this file does not match itself.
HOME = "/" + "home" + "/"
MNT = "/" + "mnt" + "/" + "c" + "/"
WINUSER = "C:" + "\\\\" + "Users"
EMAIL = "kan8223" + "@" + "gmail.com"
TEXT_PATTERNS = [
    ("absolute home path", re.compile(re.escape(HOME) + r"[A-Za-z0-9_]")),
    ("WSL Windows path", re.compile(re.escape(MNT), re.IGNORECASE)),
    ("Windows user path", re.compile(WINUSER, re.IGNORECASE)),
    ("author mail address", re.compile(re.escape(EMAIL), re.IGNORECASE)),
    ("GitHub token", re.compile(r"\b(gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})")),
    ("private key", re.compile("-----BEGIN [A-Z ]*PRIVATE " + "KEY-----")),
    ("AWS key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("API key", re.compile(r"\bsk-(ant-)?[A-Za-z0-9_-]{30,}")),
]
# Test-only features that must not be in the shipped executable.
EBOOT_FORBIDDEN = [b"AUTOPLAY", b"autoplay", b"smoke-frames", b"vita-shots", b"vita-masktest", b"TimGM6mb", b"run-logs",
                   b"vita-public-replay-menu", b"vita-test-id", b"th07-replay-probe", b"hostref", b"gxm-cleanup.log",
                   b"REPLAY SIM", b"log.txt"]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def is_binary(data):
    if b"\0" in data[:65536]:
        return True
    try:
        data.decode("utf-8")
        return False
    except UnicodeDecodeError:
        return True


def png_rgba_sha(data):
    try:
        from PIL import Image
    except ImportError:
        return None
    try:
        image = Image.open(io.BytesIO(data)).convert("RGBA")
    except Exception:
        return None
    return sha(image.tobytes())


class Audit:
    def __init__(self):
        self.failures = []
        self.checked = 0

    def fail(self, where, why):
        self.failures.append(f"{where}: {why}")

    def check(self, where, name, data, in_vpk=False):
        self.checked += 1
        lower = name.replace("\\", "/")
        if FORBIDDEN_NAME.search(lower) and lower not in CFG_WHITELIST:
            self.fail(where, "forbidden file type or location (original asset / user data / run output)")
        base = os.path.basename(lower)
        digest = sha(data)
        if base.lower().endswith(".png"):
            if digest in EXE_ICON_FILE_SHA:
                self.fail(where, "icon made from the original EXE")
            rgba = png_rgba_sha(data)
            if rgba is None:
                self.fail(where, "PNG could not be decoded (Pillow required to audit icons)")
            elif rgba in EXE_ICON_RGBA_SHA:
                self.fail(where, "icon pixels equal the original EXE icon")
        binary = is_binary(data)
        if in_vpk and lower in ("eboot.bin", "sce_sys/param.sfo"):
            self.check_strings(where, data, eboot=lower == "eboot.bin")
            return
        if binary:
            allowed = lower.endswith(ALLOWED_BINARY_SUFFIX)
            if base in ALLOWED_BINARY_NAMES:
                allowed = digest == ALLOWED_BINARY_NAMES[base]
                if not allowed:
                    self.fail(where, f"{base} differs from the pinned file")
            elif lower.endswith(".png") and (lower.endswith("assets/icon0.png") or lower == "sce_sys/icon0.png"):
                allowed = True
            if not allowed:
                self.fail(where, "unexpected binary file")
            self.check_strings(where, data, eboot=False)
            return
        text = data.decode("utf-8")
        for label, pattern in TEXT_PATTERNS:
            match = pattern.search(text)
            if match:
                line = text.count("\n", 0, match.start()) + 1
                self.fail(f"{where}:{line}", label)

    def check_strings(self, where, data, eboot):
        for label, pattern in TEXT_PATTERNS:
            if pattern.search(data.decode("latin-1")):
                self.fail(where, label + " (in binary)")
        if eboot:
            for marker in EBOOT_FORBIDDEN:
                if marker in data:
                    self.fail(where, "test-only marker " + marker.decode())


def repo_files(root):
    try:
        out = subprocess.run(["git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             check=True, capture_output=True).stdout
        return [p for p in out.decode("utf-8").split("\0") if p]
    except (subprocess.CalledProcessError, FileNotFoundError):
        files = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in (".git", "build")]
            for f in filenames:
                files.append(os.path.relpath(os.path.join(dirpath, f), root))
        return files


def git_metadata(audit, root):
    try:
        log = subprocess.run(["git", "-C", root, "log", "--all", "--format=%an <%ae>%n%cn <%ce>%n%B"],
                             check=True, capture_output=True).stdout.decode("utf-8", "replace")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return
    for label, pattern in TEXT_PATTERNS:
        if pattern.search(log):
            audit.fail("git history (authors/messages)", label)


def exe_icon_rgba(path):
    from PIL import Image
    import struct
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    data = open(path, "rb").read()
    # Same reading as touhou-modern-vita tools/extract_exe_icons.py (Pillow decodes the DIB).
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    count, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    directory = opt + {0x10b: 96, 0x20b: 112}[magic]
    res_rva, res_size = struct.unpack_from("<II", data, directory + 16)
    sections = [struct.unpack_from("<IIII", data, opt + opt_size + i * 40 + 8) for i in range(count)]

    def rva(addr):
        for vsize, start, raw_size, raw_off in sections:
            if start <= addr < start + raw_size:
                return raw_off + addr - start
        raise ValueError("rva")
    base = rva(res_rva)
    leaves = {}

    def walk(off, keys):
        named, ids = struct.unpack_from("<HH", data, base + off + 12)
        for i in range(named + ids):
            name, child = struct.unpack_from("<II", data, base + off + 16 + i * 8)
            k = keys + (name,)
            if child & 0x80000000:
                walk(child & 0x7fffffff, k)
            else:
                d_rva, d_size = struct.unpack_from("<II", data, base + child)
                leaves[k] = data[rva(d_rva):rva(d_rva) + d_size]
    walk(0, ())
    results = set()
    for (kind, group, lang), payload in leaves.items():
        if kind != 14:
            continue
        n = struct.unpack_from("<H", payload, 4)[0]
        for o in range(n):
            w, h, colors, _, planes, bpp, size, icon_id = struct.unpack_from("<BBBBHHIH", payload, 6 + o * 14)
            pixels = [v for k, v in leaves.items() if k[0] == 3 and k[1] == icon_id][0]
            ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", w, h, colors, 0, planes, bpp, size, 22) + pixels
            image = Image.open(io.BytesIO(ico)).convert("RGBA")
            results.add(sha(image.tobytes()))
            if 128 % image.width == 0:
                results.add(sha(image.resize((128, 128), Image.Resampling.NEAREST).tobytes()))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", help="source tree to audit")
    parser.add_argument("--vpk", action="append", default=[], help="VPK to audit (repeatable)")
    parser.add_argument("--elf", action="append", default=[],
                        help="uncompressed executable (build/th07 or build/th07.velf): eboot.bin in a VPK is compressed")
    parser.add_argument("--exe", help="original th07.exe / 東方妖々夢.exe: also forbid its icon pixels")
    args = parser.parse_args()
    if not args.repo and not args.vpk and not args.elf:
        parser.error("give --repo, --vpk and/or --elf")
    if args.exe:
        EXE_ICON_RGBA_SHA.update(exe_icon_rgba(args.exe))
    audit = Audit()
    if args.repo:
        files = repo_files(args.repo)
        for rel in files:
            full = os.path.join(args.repo, rel)
            if os.path.isfile(full):
                with open(full, "rb") as f:
                    audit.check("repo:" + rel, rel, f.read())
        git_metadata(audit, args.repo)
        print(f"repo {args.repo}: {len(files)} files")
    for vpk in args.vpk:
        with zipfile.ZipFile(vpk) as archive:
            names = archive.namelist()
            for name in names:
                if not name.endswith("/"):
                    audit.check("vpk:" + name, name, archive.read(name), in_vpk=True)
            if "sce_sys/icon0.png" not in names:
                audit.fail("vpk", "no sce_sys/icon0.png")
        print(f"vpk {vpk}: {len(names)} entries")
    for elf in args.elf:
        with open(elf, "rb") as f:
            audit.checked += 1
            audit.check_strings("elf:" + os.path.basename(elf), f.read(), eboot=True)
        print(f"elf {elf}: scanned")
    if audit.failures:
        print(f"FAIL ({len(audit.failures)} problems in {audit.checked} files)")
        for line in audit.failures:
            print("  " + line)
        return 1
    print(f"PASS ({audit.checked} files checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
