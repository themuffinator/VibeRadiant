#!/usr/bin/env python3
import argparse
import re
from dataclasses import dataclass
from pathlib import Path


MODEL_EXTS = {
    ".md2",
    ".md3",
    ".mdx",
    ".fm",
    ".mdl",
    ".md5mesh",
    ".md5",
    ".lwo",
    ".obj",
    ".ase",
    ".gltf",
    ".glb",
}


SECTION_KEYS_RE = re.compile(r"^-+\s*KEYS\s*-+$", re.IGNORECASE)
SECTION_SPAWNFLAGS_RE = re.compile(r"^-+\s*SPAWNFLAGS\s*-+$", re.IGNORECASE)
SECTION_NOTES_RE = re.compile(r"^-+\s*NOTES\s*-+$", re.IGNORECASE)
QUAKED_BLOCK_RE = re.compile(r"/\*QUAKED\b(.*?)\*/", re.DOTALL)


@dataclass
class EntityDef:
    classname: str
    color: tuple[int, int, int]
    mins: list[float] | None
    maxs: list[float] | None
    spawnflags: list[tuple[int, str]]
    spawnflag_desc: dict[str, str]
    keys: list[tuple[str, str]]
    description: str
    notes: list[str]
    studio: str | None


def parse_color(text: str) -> tuple[int, int, int]:
    parts = text.split()
    values = []
    for idx in range(3):
        try:
            value = float(parts[idx])
        except (IndexError, ValueError):
            value = 1.0
        value = max(0.0, min(1.0, value))
        values.append(int(round(value * 255.0)))
    return tuple(values)


def format_float(value: float) -> str:
    if abs(value - round(value)) < 1e-6:
        return str(int(round(value)))
    text = f"{value:.3f}".rstrip("0").rstrip(".")
    return text if text else "0"


def format_vec(vec: list[float]) -> str:
    return " ".join(format_float(value) for value in vec)


def parse_vec(text: str) -> list[float] | None:
    parts = text.split()
    if len(parts) < 3:
        return None
    try:
        return [float(parts[0]), float(parts[1]), float(parts[2])]
    except ValueError:
        return None


def parse_key_line(line: str) -> tuple[str | None, str]:
    match = re.match(r'^\s*"?([A-Za-z0-9_]+)"?\s*:\s*(.*)$', line)
    if match:
        return match.group(1), match.group(2).strip()
    match = re.match(r'^\s*"([A-Za-z0-9_]+)"\s+(.*)$', line)
    if match:
        return match.group(1), match.group(2).strip()
    return None, ""


def extract_model(block_text: str) -> str | None:
    for match in re.finditer(r'model\s*=\s*"([^"]+)"', block_text, re.IGNORECASE):
        candidate = match.group(1).strip().replace("\\", "/")
        for part in re.split(r"[;\s]+", candidate):
            if not part:
                continue
            ext = Path(part).suffix.lower()
            if ext in MODEL_EXTS:
                return part
    return None


def parse_spawnflags(tokens: list[str]) -> list[tuple[int, str]]:
    flags = []
    bit_index = 0
    for token in tokens:
        if not token:
            continue
        lowered = token.lower()
        if token == "?" or token == "-" or lowered == "x" or lowered.startswith("unused"):
            bit_index += 1
            continue
        flags.append((1 << bit_index, token))
        bit_index += 1
    return flags


def parse_quaked_block(block_text: str) -> EntityDef | None:
    lines = block_text.splitlines()
    if not lines:
        return None
    header = lines[0].strip()
    if header.lower().startswith("quaked"):
        header = header[len("quaked"):].strip()

    header_match = re.match(r"^(\S+)\s*\(\s*([^)]+)\s*\)\s*(.*)$", header)
    if not header_match:
        return None

    classname = header_match.group(1)
    color = parse_color(header_match.group(2))
    remainder = header_match.group(3)

    mins = None
    maxs = None
    tokens = []
    size_match = re.match(
        r"^\s*\(\s*([^)]+)\s*\)\s*\(\s*([^)]+)\s*\)\s*(.*)$", remainder
    )
    if size_match:
        mins = parse_vec(size_match.group(1))
        maxs = parse_vec(size_match.group(2))
        tokens = size_match.group(3).split()
    else:
        remainder_tokens = remainder.split()
        if remainder_tokens:
            tokens = remainder_tokens[1:]

    spawnflags = parse_spawnflags(tokens)

    description = ""
    notes: list[str] = []
    keys: list[tuple[str, str]] = []
    keys_seen: set[str] = set()
    spawnflag_desc: dict[str, str] = {}
    section = None

    for line in lines[1:]:
        raw_line = line.rstrip()
        stripped = raw_line.strip()
        if not stripped:
            if section == "notes":
                notes.append("")
            continue
        if SECTION_KEYS_RE.match(stripped):
            section = "keys"
            continue
        if SECTION_SPAWNFLAGS_RE.match(stripped):
            section = "spawnflags"
            continue
        if SECTION_NOTES_RE.match(stripped):
            section = "notes"
            continue

        if section == "spawnflags":
            if stripped.lower().startswith("(none"):
                continue
            match = re.match(r"^([A-Za-z0-9_]+)\s*[:\-]\s*(.*)$", stripped)
            if match:
                name = match.group(1)
                desc = match.group(2).strip()
                if name not in spawnflag_desc:
                    spawnflag_desc[name] = desc
            continue

        if section in ("keys", None):
            key, desc = parse_key_line(stripped)
            if key and key not in keys_seen:
                keys.append((key, desc))
                keys_seen.add(key)
                continue

        if not description:
            description = stripped
        else:
            notes.append(stripped)

    if not description:
        description = ""

    studio = extract_model(block_text)

    return EntityDef(
        classname=classname,
        color=color,
        mins=mins,
        maxs=maxs,
        spawnflags=spawnflags,
        spawnflag_desc=spawnflag_desc,
        keys=keys,
        description=description,
        notes=notes,
        studio=studio,
    )


def sanitize_text(value: str) -> str:
    replacements = {
        "\u2013": "-",
        "\u2014": "-",
        "\u2018": "'",
        "\u2019": "'",
        "\u201c": '"',
        "\u201d": '"',
        "\u00a0": " ",
    }
    for source, target in replacements.items():
        value = value.replace(source, target)
    value = value.replace('"', "'").strip()
    return value.encode("ascii", "ignore").decode("ascii")


def build_class_header(entity: EntityDef) -> str:
    class_type = "@PointClass" if entity.mins and entity.maxs else "@SolidClass"
    parts = [class_type, f"color({entity.color[0]} {entity.color[1]} {entity.color[2]})"]
    if entity.mins and entity.maxs:
        mins = format_vec(entity.mins)
        maxs = format_vec(entity.maxs)
        parts.append(f"size({mins}, {maxs})")
    if entity.studio:
        parts.append(f"studio(\"{sanitize_text(entity.studio)}\")")
    description = sanitize_text(entity.description)
    return f"{' '.join(parts)} = {entity.classname} : \"{description}\""


def write_fgd(def_path: Path, entities: list[EntityDef]) -> Path:
    output_path = def_path.with_name("entities.fgd")
    lines = [
        "// Auto-generated from entities.def.",
        "// Edit entities.def or regenerate with tools/convert_entities_def_to_fgd.py.",
        "",
    ]

    for entity in entities:
        lines.append(build_class_header(entity))
        lines.append("[")
        for note in entity.notes:
            if note:
                lines.append(f"    // {sanitize_text(note)}")
            else:
                lines.append("    //")

        if entity.spawnflags:
            lines.append("    spawnflags(Flags) =")
            lines.append("    [")
            for bit_value, name in entity.spawnflags:
                desc = sanitize_text(entity.spawnflag_desc.get(name, ""))
                if desc:
                    lines.append(f"        // {name}: {desc}")
                lines.append(f"        {bit_value} : \"{name}\" : 0")
            lines.append("    ]")

        for key, desc in entity.keys:
            desc = sanitize_text(desc)
            lines.append(f"    {key}(string) : \"{desc}\"")

        lines.append("]")
        lines.append("")

    output_path.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return output_path


def update_game_entityclasstype(game_path: Path) -> bool:
    with game_path.open("r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    match = re.search(r'(entityclasstype\s*=\s*")([^"]*)(")', text)
    if not match:
        return False
    value = match.group(2)
    tokens = value.split()
    if "def" not in tokens:
        return False
    tokens = ["fgd" if token == "def" else token for token in tokens]
    replacement = f'{match.group(1)}{" ".join(tokens)}{match.group(3)}'
    updated = text[: match.start()] + replacement + text[match.end() :]
    if updated == text:
        return False
    with game_path.open("w", encoding="utf-8", newline="") as handle:
        handle.write(updated)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert entities.def to entities.fgd")
    parser.add_argument(
        "--root",
        default="games/VibePack",
        help="Root directory to scan for entities.def",
    )
    args = parser.parse_args()

    root = Path(args.root)
    def_files = sorted(root.rglob("entities.def"))
    if not def_files:
        print("No entities.def files found.")
        return 1

    game_files = set()
    for def_path in def_files:
        text = def_path.read_text(encoding="utf-8", errors="replace")
        entities = []
        for block_match in QUAKED_BLOCK_RE.finditer(text):
            entity = parse_quaked_block(block_match.group(1))
            if entity:
                entities.append(entity)
        if not entities:
            print(f"Skipping {def_path}: no QUAKED blocks found.")
            continue
        output_path = write_fgd(def_path, entities)
        print(f"Wrote {output_path}")
        game_files.add(def_path.parent.parent.name)

    for game_name in sorted(game_files):
        game_path = Path("games/VibePack/games") / game_name
        if not game_path.exists():
            print(f"Missing game file: {game_path}")
            continue
        if update_game_entityclasstype(game_path):
            print(f"Updated {game_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
