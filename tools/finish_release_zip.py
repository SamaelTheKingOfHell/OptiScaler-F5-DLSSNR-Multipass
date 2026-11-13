#!/usr/bin/env python3
"""Turns package_release.ps1's zip into the one players download.

package_release.ps1 refuses a zip with a frame generation output already chosen, so that an ordinary package can never
ship one by accident. A release does ship one: F5Low with XeFG Tensor. This sets those two keys, updates SHA256SUMS.txt
to match, checks every listed file against its hash and writes the zip under the release's short name.

    python tools/finish_release_zip.py <package zip> <output zip>
"""

import hashlib
import re
import sys
import zipfile

DEFAULTS = ((b"FGOutput", b"f5low"), (b"FrameGenerator", b"xefg_tensor"))


def finished_ini(ini: bytes) -> bytes:
    for key, value in DEFAULTS:
        old = b"\r\n" + key + b"=auto\r\n"
        if ini.count(old) != 1:
            raise SystemExit(f"REFUSING: the ini must have {key.decode()}=auto exactly once")
        ini = ini.replace(old, b"\r\n" + key + b"=" + value + b"\r\n")
    return ini


def finished_sums(sums: str, ini: bytes) -> str:
    digest = hashlib.sha256(ini).hexdigest().upper()
    sums, count = re.subn(r"(?m)^[0-9A-F]{64}( \*OptiScaler\.ini\r?)$", lambda m: digest + m.group(1), sums)
    if count != 1:
        raise SystemExit("REFUSING: SHA256SUMS.txt must list OptiScaler.ini exactly once")
    return sums


def check_sums(path: str) -> int:
    with zipfile.ZipFile(path) as package:
        if package.testzip() is not None:
            raise SystemExit("REFUSING: the finished zip is damaged")
        checked = 0
        for line in package.read("SHA256SUMS.txt").decode().splitlines():
            match = re.match(r"([0-9A-F]{64}) \*(.+)", line)
            if not match:
                continue
            if hashlib.sha256(package.read(match.group(2))).hexdigest().upper() != match.group(1):
                raise SystemExit(f"REFUSING: {match.group(2)} does not match SHA256SUMS.txt")
            checked += 1
    if checked == 0:
        raise SystemExit("REFUSING: SHA256SUMS.txt lists no files")
    return checked


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    source, output = sys.argv[1:]

    with zipfile.ZipFile(source) as package, zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED, compresslevel=6) as out:
        ini = finished_ini(package.read("OptiScaler.ini"))
        sums = finished_sums(package.read("SHA256SUMS.txt").decode(), ini)
        for entry in package.infolist():
            data = package.read(entry.filename)
            if entry.filename == "OptiScaler.ini":
                data = ini
            elif entry.filename == "SHA256SUMS.txt":
                data = sums.encode()
            out.writestr(entry, data, compress_type=entry.compress_type)

    print(f"{output}: {check_sums(output)} files match SHA256SUMS.txt")


if __name__ == "__main__":
    main()
