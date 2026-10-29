#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Audit actual versioned PE exports against upstream specs and game imports.

An export match verifies the ABI surface only, not implementation completeness.
Functional coverage belongs to the compiled shader/texture/render fixture.
"""
import argparse
import hashlib
import json
import pathlib
import re

from importlib.util import module_from_spec, spec_from_file_location
from wine_source import resolve_wine_source

location = pathlib.Path(__file__).resolve().parent
spec = spec_from_file_location("game_import_inventory", location / "inventory-game-imports.py")
inventory_module = module_from_spec(spec)
spec.loader.exec_module(inventory_module)
PE = inventory_module.PE


def exports(pe):
    address, size = pe.directories[0]
    if not address or size < 40:
        raise ValueError("missing export directory")
    fields = pe.unpack("IIHHIIIIIII", pe.offset(address, 40))
    count, names, ordinals, functions = fields[7], fields[9], fields[10], fields[8]
    base, function_count = fields[5], fields[6]
    result = {}
    if count > 65536 or function_count > 65536:
        raise ValueError("excessive export count")
    for index in range(count):
        name_rva = pe.unpack("I", pe.offset(names + index * 4, 4))[0]
        ordinal = pe.unpack("H", pe.offset(ordinals + index * 2, 2))[0]
        if ordinal >= function_count:
            raise ValueError("export ordinal outside function table")
        function = pe.unpack("I", pe.offset(functions + ordinal * 4, 4))[0]
        if not function:
            raise ValueError("named export has no function")
        result[pe.string(name_rva)] = base + ordinal
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=pathlib.Path, required=True)
    parser.add_argument("--inventory", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--wine-source-directory", type=pathlib.Path)
    args = parser.parse_args()
    wine_source = resolve_wine_source(args.wine_source_directory)
    inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
    results = []
    for module in ("d3dx9_37", "d3dx9_41", "d3dx9_42", "d3dcompiler_42"):
        spec_text = (wine_source / "dlls" / module / (module + ".spec")).read_text()
        expected = set(re.findall(r"^\s*(?:@|\d+)\s+\w+\s+(?:-\S+\s+)*([A-Za-z_]\w*)", spec_text, re.M))
        if not expected:
            raise ValueError("empty upstream spec: " + module)
        for arch, machine in (("x86", 0x14c), ("x64", 0x8664)):
            path = args.runtime.resolve() / "builtin" / arch / (module + ".dll")
            data = path.read_bytes()
            pe = PE(data)
            actual = exports(pe)
            if pe.machine != machine or set(actual) != expected:
                raise ValueError(f"{path}: machine={pe.machine:#x}, missing={expected-set(actual)}, extra={set(actual)-expected}")
            dependencies = [item["dll"] for item in pe.imports()]
            if any(re.search(r"msys|cygwin|libgcc|libstdc|libwinpthread", name, re.I) for name in dependencies):
                raise ValueError("non-native runtime dependency: " + str(path))
            imported = [entry for entry in inventory["helper_imports"].get(module + ".dll", []) if entry["architecture"] == arch and not entry["redistributable"]]
            for entry in imported:
                for name in entry["symbols"]:
                    if name.startswith("#"):
                        if int(name[1:]) not in actual.values():
                            raise ValueError("game imports absent ordinal: " + name)
                    elif name not in actual:
                        raise ValueError("game imports absent export: " + name)
            results.append({"module": module, "architecture": arch, "path": str(path),
                            "sha256": hashlib.sha256(data).hexdigest(), "exports_match_spec": True,
                            "export_count": len(actual), "imports": dependencies,
                            "matched_game_imports": imported})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"passed": True, "modules": results,
                                      "scope": "ABI surface and PE dependencies; function bodies require runtime probes"}, indent=2), encoding="utf-8")
    print(f"PASS {len(results)} module/architecture export and native-dependency audits: {args.output}")


if __name__ == "__main__":
    main()
