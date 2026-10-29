#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Read-only PE import inventory, including PE members inside game ZIP files.

Archives are never extracted or modified. Dynamic-name strings are reported
separately: their presence is not evidence that Windows actually imports a DLL.
"""
import argparse
import collections
import hashlib
import json
import pathlib
import re
import struct
import time
import zipfile

HELPER = re.compile(r"^(?:d3dx(?:9|10|11)(?:_\d+)?|d3dcompiler(?:_\d+)?|d3dcsx(?:_\d+)?|d3dxof)\.dll$", re.I)
HELPER_STRING = re.compile(rb"\b(?:d3dx(?:9|10|11)(?:_\d+)?|d3dcompiler(?:_\d+)?|d3dcsx(?:_\d+)?|d3dxof)\.dll\b", re.I)
REDIST = re.compile(r"(?:^|[/\\])(?:_?commonredist|redist|redistributables?|directx|vcredist|dotnet|installers?)(?:[/\\]|$)|(?:^|[/\\])(?:dxsetup|vcredist[^/\\]*|unins[^/\\]*)\.exe$", re.I)


class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ image")
        nt = self.unpack("I", 0x3C)[0]
        if data[nt:nt + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        self.machine, count, _, _, _, optional_size, self.flags = self.unpack("HHIIIHH", nt + 4)
        optional = nt + 24
        magic = self.unpack("H", optional)[0]
        if magic == 0x10B:
            self.width, directory_offset, count_offset = 4, 96, 92
            self.image_base = self.unpack("I", optional + 28)[0]
        elif magic == 0x20B:
            self.width, directory_offset, count_offset = 8, 112, 108
            self.image_base = self.unpack("Q", optional + 24)[0]
        else:
            raise ValueError("unsupported optional header")
        if optional_size < directory_offset or count > 128:
            raise ValueError("unsupported header bounds")
        self.headers = self.unpack("I", optional + 60)[0]
        directory_count = self.unpack("I", optional + count_offset)[0]
        self.directories = []
        for index in range(min(directory_count, 16)):
            if directory_offset + (index + 1) * 8 > optional_size:
                raise ValueError("truncated directory table")
            self.directories.append(self.unpack("II", optional + directory_offset + index * 8))
        self.sections = []
        for index in range(count):
            at = optional + optional_size + index * 40
            _, virtual_size, address, raw_size, raw_offset = self.unpack("8sIIII", at)
            self.sections.append((address, virtual_size, raw_size, raw_offset))

    def unpack(self, pattern, offset):
        size = struct.calcsize("<" + pattern)
        if offset < 0 or offset + size > len(self.data):
            raise ValueError("structure outside image")
        return struct.unpack_from("<" + pattern, self.data, offset)

    def offset(self, address, size=1):
        if address < self.headers and address + size <= min(self.headers, len(self.data)):
            return address
        for base, _, raw_size, raw_offset in self.sections:
            relative = address - base
            if 0 <= relative and relative + size <= raw_size:
                offset = raw_offset + relative
                if offset + size <= len(self.data):
                    return offset
        raise ValueError("RVA outside raw image")

    def string(self, address, maximum=32768):
        at = self.offset(address)
        end = self.data.find(b"\0", at, min(len(self.data), at + maximum))
        if end < 0:
            raise ValueError("unterminated PE string")
        return self.data[at:end].decode("ascii", errors="backslashreplace")

    def symbols(self, address):
        names = []
        for index in range(32768):
            value = self.unpack("I" if self.width == 4 else "Q", self.offset(address + index * self.width, self.width))[0]
            if value == 0:
                return names
            if value & (1 << (self.width * 8 - 1)):
                names.append("#" + str(value & 0xFFFF))
            else:
                names.append(self.string(value + 2))
        raise ValueError("excessive import symbols")

    def imports(self, delay=False):
        index = 13 if delay else 1
        if index >= len(self.directories):
            return []
        address, size = self.directories[index]
        if not address:
            return []
        result = []
        width = 32 if delay else 20
        for index in range(min(4096, max(size // width, 1))):
            fields = self.unpack("8I" if delay else "5I", self.offset(address + index * width, width))
            if not any(fields):
                return result
            if delay:
                attributes, name, _, iat, lookup, _, _, _ = fields
                if not attributes & 1:
                    name -= self.image_base
                    lookup = lookup - self.image_base if lookup else iat - self.image_base
                else:
                    lookup = lookup or iat
            else:
                lookup, timestamp, _, name, iat = fields
                lookup = lookup or iat
            dll = self.string(name).lower()
            item = {"dll": dll}
            if HELPER.match(pathlib.PureWindowsPath(dll).name):
                try:
                    item["symbols"] = self.symbols(lookup)
                except ValueError as error:
                    item["symbols_error"] = str(error)
            result.append(item)
        raise ValueError("unterminated import directory")


def game_titles(index):
    titles, key = {}, None
    if not index.is_file():
        return titles
    for line in index.read_text(encoding="utf-8-sig").splitlines():
        match = re.match(r"^  ([0-9a-f-]{36}):\s*$", line)
        if match:
            key = match[1]
        match = re.match(r"^    title:\s*(.+)$", line)
        if match and key:
            value = match[1].strip()
            if value.startswith("'") and value.endswith("'"):
                value = value[1:-1].replace("''", "'")
            elif value.startswith('"'):
                value = json.loads(value)
            titles[key] = value
    return titles


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--max-member-mib", type=int, default=256)
    parser.add_argument("--max-total-mib", type=int, default=8192)
    args = parser.parse_args()
    root = args.root.resolve()
    output = args.output.resolve()
    if output.is_relative_to(root):
        raise SystemExit("Output must be outside the read-only inventory source")
    titles = game_titles(root / "archive" / "index.yaml")
    records, errors, skipped, archives = [], [], [], []
    total = 0
    started = time.time()

    def inspect(data, origin):
        try:
            pe = PE(data)
            record = dict(origin, architecture={0x14C: "x86", 0x8664: "x64", 0xAA64: "arm64"}.get(pe.machine, hex(pe.machine)),
                          bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                          imports=pe.imports(), delay_imports=pe.imports(True),
                          managed=len(pe.directories) > 14 and pe.directories[14][0] != 0)
            record["helper_name_strings"] = sorted({m[0].decode("ascii").lower() for m in HELPER_STRING.finditer(data)})
            wide = data.decode("utf-16le", errors="ignore").encode("ascii", errors="ignore")
            record["helper_name_strings"] = sorted(set(record["helper_name_strings"]) | {m[0].decode("ascii").lower() for m in HELPER_STRING.finditer(wide)})
            records.append(record)
        except (ValueError, struct.error) as error:
            errors.append(dict(origin, error=str(error)))

    archive_root = root / "archive"
    for archive in sorted(archive_root.rglob("*.zip")):
        relative = archive.relative_to(archive_root)
        game_id = relative.parts[0]
        title = titles.get(game_id, game_id)
        version = relative.parts[1] if len(relative.parts) > 1 else ""
        before = archive.stat()
        count = 0
        try:
            with zipfile.ZipFile(archive) as zipped:
                for member in zipped.infolist():
                    if pathlib.PurePosixPath(member.filename.replace("\\", "/")).suffix.lower() not in (".exe", ".dll"):
                        continue
                    origin = {"game": title, "game_id": game_id, "version": version, "archive": str(archive), "member": member.filename,
                              "redistributable": bool(REDIST.search(member.filename))}
                    if member.file_size > args.max_member_mib * 1024 * 1024 or total + member.file_size > args.max_total_mib * 1024 * 1024:
                        skipped.append(dict(origin, bytes=member.file_size, reason="inventory size budget"))
                        continue
                    try:
                        with zipped.open(member) as source:
                            data = source.read(args.max_member_mib * 1024 * 1024 + 1)
                        if len(data) != member.file_size:
                            raise ValueError("archive member size differs from directory entry")
                        total += len(data)
                        inspect(data, origin)
                        count += 1
                    except (RuntimeError, NotImplementedError, ValueError, zipfile.BadZipFile) as error:
                        errors.append(dict(origin, error=str(error)))
        except (OSError, zipfile.BadZipFile) as error:
            errors.append({"archive": str(archive), "error": str(error)})
        after = archive.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise RuntimeError("Source archive changed during inventory: " + str(archive))
        archives.append({"path": str(archive), "bytes": before.st_size, "mtime_ns": before.st_mtime_ns, "pe_members_read": count})
        if count:
            print(f"{title}: {count} executable/library members read; total {len(records)} PE images", flush=True)

    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".exe", ".dll"):
            continue
        size = path.stat().st_size
        if size > args.max_member_mib * 1024 * 1024 or total + size > args.max_total_mib * 1024 * 1024:
            skipped.append({"file": str(path), "bytes": size, "reason": "inventory size budget"})
            continue
        total += size
        inspect(path.read_bytes(), {"file": str(path), "game": "Loose dependency/component", "redistributable": True})

    summary = collections.defaultdict(list)
    for record in records:
        for kind in ("imports", "delay_imports"):
            for item in record[kind]:
                if HELPER.match(pathlib.PureWindowsPath(item["dll"]).name):
                    summary[item["dll"]].append({"game": record["game"], "architecture": record["architecture"],
                        "member": record.get("member", record.get("file")), "kind": kind,
                        "redistributable": record["redistributable"], "symbols": item.get("symbols", [])})
    result = {"root": str(root), "read_only": True, "elapsed_seconds": round(time.time() - started, 3),
              "uncompressed_pe_bytes_read": total, "archives": archives, "images": records, "errors": errors,
              "skipped": skipped, "helper_imports": dict(sorted(summary.items()))}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps({"output": str(output), "images": len(records), "errors": len(errors), "skipped": len(skipped),
                      "helper_modules": sorted(summary), "bytes_read": total}), flush=True)


if __name__ == "__main__":
    main()
