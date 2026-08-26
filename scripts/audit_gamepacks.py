#!/usr/bin/env python3
"""Audit VibeRadiant gamepack source layout and descriptor quality."""

from __future__ import annotations

import argparse
import hashlib
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


REQUIRED_ATTRS = {
    "type",
    "name",
    "baseGame",
    "archiveTypes",
    "mapTypes",
    "entityClass",
    "entities",
    "brushTypes",
    "patchTypes",
}

RECOMMENDED_ATTRS = {
    "baseGameName",
    "unknownGameName",
    "installAliases",
    "knownMods",
    "knownModNames",
}


def _parse_descriptor(path: Path) -> dict[str, str]:
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        raise ValueError(f"{path}: invalid XML ({exc})") from exc
    if root.tag != "game":
        raise ValueError(f"{path}: root tag must be <game>, got <{root.tag}>")
    return dict(root.attrib)


def _normalise_key(name: str) -> str:
    return "".join(ch.lower() for ch in name if ch.isalnum())


def _get_attr(attrs: dict[str, str], key: str) -> str:
    value = attrs.get(key, "")
    if value.strip():
        return value

    wanted = _normalise_key(key)
    for attr_key, attr_value in attrs.items():
        if _normalise_key(attr_key) == wanted:
            return attr_value
    return ""


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _audit_pack(pack: Path, strict_no_legacy_def: bool = False) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    games_dir = pack / "games"
    if not games_dir.is_dir():
        errors.append(f"{pack}: missing games/ descriptor directory")
        return errors, warnings

    descriptors = sorted(games_dir.glob("*.game"))
    if not descriptors:
        errors.append(f"{pack}: no descriptors found in {games_dir}")
        return errors, warnings

    descriptor_case_map: dict[str, str] = {}
    for desc in descriptors:
        rel = desc.relative_to(pack).as_posix()
        case_key = desc.name.lower()
        previous = descriptor_case_map.get(case_key)
        if previous is not None and previous != desc.name:
            errors.append(
                f"{pack}: duplicate descriptor names differing only by case: {previous} vs {desc.name}"
            )
        descriptor_case_map[case_key] = desc.name

        payload_dir = pack / desc.name
        if not payload_dir.is_dir():
            errors.append(f"{pack}: missing payload directory for {rel}: {payload_dir.name}/")
            continue

        try:
            attrs = _parse_descriptor(desc)
        except ValueError as exc:
            errors.append(str(exc))
            continue

        missing_required = sorted(k for k in REQUIRED_ATTRS if not _get_attr(attrs, k).strip())
        if missing_required:
            errors.append(f"{rel}: missing required attrs: {', '.join(missing_required)}")

        missing_recommended = sorted(k for k in RECOMMENDED_ATTRS if not _get_attr(attrs, k).strip())
        if missing_recommended:
            warnings.append(f"{rel}: missing recommended attrs: {', '.join(missing_recommended)}")

        if strict_no_legacy_def:
            entityclasstype_tokens = _get_attr(attrs, "entityClassType").split()
            if "def" in entityclasstype_tokens:
                errors.append(f"{rel}: legacy entityclasstype token 'def' is not allowed")

            legacy_defs = sorted(p.relative_to(pack).as_posix() for p in payload_dir.rglob("*.def"))
            if legacy_defs:
                errors.append(
                    f"{rel}: legacy .def files remain under payload: {', '.join(legacy_defs)}"
                )

    payload_dirs = {p.name for p in pack.glob("*.game") if p.is_dir()}
    descriptor_names = {d.name for d in descriptors}
    orphaned = sorted(payload_dirs - descriptor_names)
    for item in orphaned:
        warnings.append(f"{pack}: payload directory has no descriptor: {item}/")

    return errors, warnings


def _compare_packs(left: Path, right: Path) -> list[str]:
    notes: list[str] = []

    left_files = {
        p.relative_to(left).as_posix(): p
        for p in left.rglob("*")
        if p.is_file()
    }
    right_files = {
        p.relative_to(right).as_posix(): p
        for p in right.rglob("*")
        if p.is_file()
    }

    only_left = sorted(set(left_files) - set(right_files))
    only_right = sorted(set(right_files) - set(left_files))
    shared = sorted(set(left_files) & set(right_files))

    if only_left:
        notes.append(f"Only in {left}:")
        notes.extend(f"  {item}" for item in only_left)
    if only_right:
        notes.append(f"Only in {right}:")
        notes.extend(f"  {item}" for item in only_right)

    different = [item for item in shared if _sha256(left_files[item]) != _sha256(right_files[item])]
    if different:
        notes.append(f"Differing files between {left} and {right}:")
        notes.extend(f"  {item}" for item in different)

    if not notes:
        notes.append(f"{left} and {right} are identical.")

    return notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        default="games/VibePack",
        help="Primary source pack to audit (default: games/VibePack)",
    )
    parser.add_argument(
        "--compare-with",
        default="",
        help="Optional secondary pack to compare drift against (for example: games/NRCPack)",
    )
    parser.add_argument(
        "--no-compare",
        action="store_true",
        help="Skip source pack comparison output",
    )
    parser.add_argument(
        "--enforce-no-legacy-def",
        action="store_true",
        help="Treat legacy entityclasstype='def' and payload .def files as errors",
    )
    args = parser.parse_args()

    source = Path(args.source)
    compare = Path(args.compare_with) if args.compare_with else None

    errors, warnings = _audit_pack(source, strict_no_legacy_def=args.enforce_no_legacy_def)

    print(f"Audited source pack: {source}")
    if errors:
        print("Errors:")
        for item in errors:
            print(f"  - {item}")
    else:
        print("Errors: none")

    if warnings:
        print("Warnings:")
        for item in warnings:
            print(f"  - {item}")
    else:
        print("Warnings: none")

    if not args.no_compare and compare is not None and compare.exists():
        print("")
        print("Pack drift report:")
        for line in _compare_packs(source, compare):
            print(line)
    elif not args.no_compare and compare is not None:
        print("")
        print(f"Compare pack not found, skipping drift report: {compare}")

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
