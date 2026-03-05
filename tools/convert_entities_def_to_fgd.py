#!/usr/bin/env python3
"""Convert legacy .def entity definitions to richer .fgd definitions.

The converter scans all ``*.def`` files under a gamepack root, extracts QUAKED
blocks, and writes sibling ``*.fgd`` files. It can also rewrite game
descriptors to replace ``entityclasstype="... def ..."` with ``fgd`` and
optionally remove legacy ``.def`` files.
"""

from __future__ import annotations

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
CHOICE_RE = re.compile(
    r"(?<!\w)(-?\d+)\s*[:=]\s*([A-Za-z0-9][A-Za-z0-9_./() \-]{0,64}?)(?=\s+-?\d+\s*[:=]|\s*$)"
)


TARGET_KEYS = {
    "target",
    "target2",
    "target3",
    "target4",
    "killtarget",
    "pathtarget",
    "brushparent",
    "brushchild",
    "teamchain",
    "teammaster",
    "paintarget",
    "combattarget",
    "deathtarget",
    "guard_target",
    "leader_target",
    "health_target",
    "health_target2",
    "health_target3",
    "master",
    "master_name",
    "master_nameof",
    "globalstate",
}

TARGETNAME_KEYS = {"targetname", "targetname2", "_targetname", "npc_targetname"}
MODEL_KEYS = {"model", "model2", "mdl", "mdl_dead", "debris", "shootmodel", "gibmodel"}
SOUND_KEYS = {
    "noise",
    "noise1",
    "noise2",
    "noise_start",
    "noise_stop",
    "sound",
    "soundset",
    "rotatesound",
    "messagesound",
    "idlesound",
    "music",
}
TEXTURE_KEYS = {"texture", "shader", "material", "sky"}
SKIN_KEYS = {"skin"}
COLOR_KEYS = {"_color", "color", "flashcolor", "normalcolor", "suncolor", "ambientcolor"}
VECTOR_KEYS = {
    "origin",
    "velocity",
    "angles1",
    "angles2",
    "angles3",
    "mins",
    "maxs",
    "size",
    "debrisvelocity",
    "debrisvelocityjitter",
    "debrisavelocityjitter",
    "light_radius",
    "skyaxis",
}
BOOLEAN_KEYS = {
    "notfree",
    "notduel",
    "notteam",
    "notsingle",
    "notbot",
    "notqf",
    "notctf",
    "nottdm",
    "start_open",
    "start_off",
    "toggle",
    "spawn_disabled",
    "nobots",
    "nohumans",
    "inactive",
    "not_player",
    "triggered",
    "any",
    "crusher",
    "cinematic",
    "walking",
    "fixed",
    "stalk",
    "coward",
    "ambush",
    "asleep",
    "wander",
    "once_only",
    "ffa",
    "tourney",
    "single",
    "ca",
    "ctf",
    "oneflag",
    "overload",
    "harvester",
    "ft",
}

INTEGER_KEYS = {
    "count",
    "health",
    "dmg",
    "sounds",
    "style",
    "team",
    "gametype",
    "spawnflags",
    "cnt",
    "damage",
    "head",
    "body",
    "renderamt",
    "mintel",
    "currentcash",
    "health_threshold",
    "health_threshold2",
    "health_threshold3",
    "sounds",
    "not_gametype",
}

REAL_KEYS = {
    "speed",
    "wait",
    "random",
    "delay",
    "lip",
    "height",
    "radius",
    "distance",
    "accel",
    "decel",
    "volume",
    "gravity",
    "mass",
    "phase",
    "scale",
    "light",
    "_cone",
    "duration",
    "amount",
    "pitch",
    "roll",
    "rotation",
    "framerate",
    "framestart",
    "friction",
    "imagnitude",
    "noiseamplitude",
    "texturescroll",
    "alphalevel",
    "_lightmapscale",
    "maxrange",
    "yawrate",
    "yawrange",
    "melee_range",
    "missile_range",
    "min_missile_range",
    "jump_chance",
}

SKIP_KEYS = {
    "notes",
    "note",
    "spawnflags",
    "keys",
    "model_for_radiant_only_do_not_set_this_as_a_key",
}


@dataclass
class KeyDef:
    name: str
    display_name: str
    description: str
    key_type: str
    choices: list[tuple[str, str]]
    default_value: str | None = None


@dataclass
class EntityDef:
    classname: str
    color: tuple[int, int, int]
    mins: list[float] | None
    maxs: list[float] | None
    spawnflags: list[tuple[int, str]]
    spawnflag_desc: dict[str, str]
    keys: list[KeyDef]
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
    match = re.match(r'^\s*"?([A-Za-z0-9_\.\-]+)"?\s*:\s*(.*)$', line)
    if match:
        return match.group(1), match.group(2).strip()
    match = re.match(r'^\s*"?([A-Za-z0-9_\.\-]+)"?\s*-{2,}\s*(.*)$', line)
    if match:
        return match.group(1), match.group(2).strip()
    match = re.match(r'^\s*"?([A-Za-z0-9_\.\-]+)"?\s*=\s*(.*)$', line)
    if match:
        return match.group(1), match.group(2).strip()
    match = re.match(r'^\s*"([A-Za-z0-9_\.\-]+)"\s+(.*)$', line)
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


def pretty_key_name(key: str) -> str:
    raw = key.replace("_", " ").replace(".", " ")
    words = raw.split()
    if not words:
        return key
    return " ".join(word if word.isupper() else word.capitalize() for word in words)


def should_skip_key(key: str, desc: str) -> bool:
    k = key.lower()
    if k in SKIP_KEYS:
        return True
    if re.fullmatch(r"[-_]+", k):
        return True
    d = desc.lower()
    if "do not set this as a key" in d:
        return True
    return False


def infer_default_value(desc: str) -> str | None:
    match = re.search(
        r"\bdefault(?:\s+is|\s+of)?\s*[:=]?\s*(-?\d+(?:\.\d+)?)\b",
        desc,
        flags=re.IGNORECASE,
    )
    if match:
        return match.group(1)
    return None


def _numeric_kind(value: str | None) -> str | None:
    if value is None:
        return None
    text = value.strip()
    if not text:
        return None
    if re.fullmatch(r"-?\d+", text):
        return "integer"
    if re.fullmatch(r"-?(?:\d+\.\d+|\d+)", text):
        return "real"
    return None


def extract_choices(desc: str) -> list[tuple[str, str]]:
    if not desc:
        return []

    choices: list[tuple[str, str]] = []
    seen: set[str] = set()

    for match in CHOICE_RE.finditer(desc):
        value = sanitize_text(match.group(1))
        label = sanitize_text(match.group(2).strip(" .,;:"))
        if not label:
            continue
        if value in seen:
            continue
        seen.add(value)
        choices.append((value, label))

    if len(choices) < 2:
        return []
    return choices


def infer_key_type_by_name(key: str, desc: str) -> str | None:
    k = key.lower()
    d = desc.lower()

    if k in TARGET_KEYS or (k.endswith("target") and "targetname" not in k) or re.search(r"_target\d*$", k):
        return "target"
    if k in TARGETNAME_KEYS or "targetname" in k:
        return "targetname"
    if k == "angles":
        return "angles"
    if k == "angle":
        return "direction"
    if k in MODEL_KEYS:
        return "model"
    if k in SOUND_KEYS:
        return "sound"
    if k in TEXTURE_KEYS:
        return "texture"
    if k in SKIN_KEYS:
        return "skin"
    if k in COLOR_KEYS:
        return "color"
    if k == "size":
        if "vector" in d or "x y z" in d or "mins" in d or "maxs" in d:
            return "vector3"
        return None
    if k in VECTOR_KEYS:
        return "vector3"
    if k in BOOLEAN_KEYS or k.endswith("_disabled"):
        return "boolean"
    if k.startswith("not") and not k.startswith("note") and k != "not_gametype":
        return "boolean"
    if (
        k.startswith("no")
        and not k.startswith("not")
        and len(k) > 2
        and k[2].isalpha()
        and not k.startswith("noise")
    ):
        return "boolean"
    if k in INTEGER_KEYS:
        return "integer"
    if k in REAL_KEYS:
        return "real"
    if k.endswith(("count", "style", "threshold", "damage", "dmg", "health", "head", "body", "cash", "intel")):
        return "integer"
    if k.endswith(
        (
            "speed",
            "wait",
            "delay",
            "radius",
            "height",
            "distance",
            "scale",
            "time",
            "duration",
            "amount",
            "pitch",
            "roll",
            "rotation",
            "chance",
            "range",
            "rate",
            "alpha",
        )
    ):
        return "real"
    if k.endswith("axis"):
        return "vector3"
    if k.endswith("color"):
        return "color"
    return None


def infer_key_type(key: str, desc: str, choices: list[tuple[str, str]], default_value: str | None = None) -> str:
    d = desc.lower()

    inferred_from_name = infer_key_type_by_name(key, desc)
    if inferred_from_name:
        return inferred_from_name

    if choices:
        values = {value for value, _ in choices}
        if values <= {"0", "1"} and len(values) == 2:
            return "boolean"
        return "choices"

    numeric_default = _numeric_kind(default_value)
    if numeric_default:
        return numeric_default

    if "pitch yaw roll" in d:
        return "angles"
    if "vector" in d or "x y z" in d:
        return "vector3"
    if re.search(r"\bname of (an|the) entity\b", d) and any(
        marker in d for marker in ("target", "points to", "path_corner", "multisource", "to use")
    ):
        return "target"
    if re.search(r"\bname of (this|the) entity\b", d):
        return "targetname"
    if "rgb" in d or "color" in d:
        return "color"
    if ".wav" in d or ".ogg" in d or "sound to" in d:
        return "sound"
    if any(ext in d for ext in (".md2", ".md3", ".md5", ".ase", ".obj", ".lwo", ".fm")):
        return "model"
    if "shader" in d or "texture" in d or "material" in d:
        return "texture"
    if re.search(r"\b(set to|set this to)\s*1\b", d):
        return "boolean"
    if re.search(r"\b0\s*[:=]\s*(off|no|false)\b", d) and re.search(r"\b1\s*[:=]\s*(on|yes|true)\b", d):
        return "boolean"
    if re.search(r"\b(integer|whole number)\b", d):
        return "integer"
    if re.search(r"\b(float|decimal|seconds?)\b", d):
        return "real"
    numeric_desc = _numeric_kind(desc)
    if numeric_desc:
        return numeric_desc
    return "string"


def parse_quaked_block(block_text: str) -> EntityDef | None:
    lines = block_text.splitlines()
    if not lines:
        return None
    header = lines[0].strip()
    if header.lower().startswith("quaked"):
        header = header[len("quaked") :].strip()

    header_match = re.match(r"^(\S+)\s*\(\s*([^)]+)\s*\)\s*(.*)$", header)
    if not header_match:
        return None

    classname = header_match.group(1)
    color = parse_color(header_match.group(2))
    remainder = header_match.group(3)

    mins = None
    maxs = None
    tokens = []
    size_match = re.match(r"^\s*\(\s*([^)]+)\s*\)\s*\(\s*([^)]+)\s*\)\s*(.*)$", remainder)
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
    raw_keys: list[tuple[str, str]] = []
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
                raw_keys.append((key, desc))
                keys_seen.add(key)
                continue

        if not description:
            description = stripped
        else:
            notes.append(stripped)

    studio = extract_model(block_text)

    keys: list[KeyDef] = []
    for key, desc in raw_keys:
        if should_skip_key(key, desc):
            continue
        choices = extract_choices(desc)
        default_value = infer_default_value(desc)
        key_type = infer_key_type(key, desc, choices, default_value)
        keys.append(
            KeyDef(
                name=key,
                display_name=pretty_key_name(key),
                description=sanitize_text(desc),
                key_type=key_type,
                choices=choices,
                default_value=default_value,
            )
        )

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


def _write_typed_key(lines: list[str], key: KeyDef) -> None:
    key_type = key.key_type if key.key_type != "choices" else "choices"
    if key_type == "choices" and key.choices:
        default_value = key.default_value if key.default_value is not None else key.choices[0][0]
        lines.append(
            f'    {key.name}(choices) : "{sanitize_text(key.display_name)}" : {default_value} : "{key.description}" ='
        )
        lines.append("    [")
        for value, label in key.choices:
            lines.append(f'        {sanitize_text(value)} : "{sanitize_text(label)}"')
        lines.append("    ]")
        return

    default_suffix = ""
    if key.default_value is not None:
        default_suffix = f" : {sanitize_text(key.default_value)}"
    description_suffix = f' : "{key.description}"' if key.description else ""
    lines.append(
        f'    {key.name}({key_type}) : "{sanitize_text(key.display_name)}"{default_suffix}{description_suffix}'
    )


def write_fgd(def_path: Path, entities: list[EntityDef]) -> Path:
    output_path = def_path.with_suffix(".fgd")
    lines = [
        f"// Auto-generated from {def_path.name}.",
        "// Regenerate with tools/convert_entities_def_to_fgd.py.",
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
            lines.append("    spawnflags(flags) =")
            lines.append("    [")
            for bit_value, name in entity.spawnflags:
                desc = sanitize_text(entity.spawnflag_desc.get(name, ""))
                if desc:
                    lines.append(f"        // {sanitize_text(name)}: {desc}")
                lines.append(f'        {bit_value} : "{sanitize_text(name)}" : 0')
            lines.append("    ]")

        for key in entity.keys:
            _write_typed_key(lines, key)

        lines.append("]")
        lines.append("")

    output_path.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return output_path


def update_game_entityclasstype(game_path: Path) -> bool:
    text = game_path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r'((?:entityclasstype|entityClassType)\s*=\s*")([^"]*)(")', text)
    if not match:
        return False

    tokens = [token for token in match.group(2).split() if token]
    if "def" not in tokens:
        return False

    updated_tokens: list[str] = []
    for token in tokens:
        if token == "def":
            token = "fgd"
        if token not in updated_tokens:
            updated_tokens.append(token)

    if "fgd" not in updated_tokens:
        updated_tokens.insert(0, "fgd")

    replacement = f'{match.group(1)}{" ".join(updated_tokens)}{match.group(3)}'
    updated = text[: match.start()] + replacement + text[match.end() :]
    if updated == text:
        return False
    game_path.write_text(updated, encoding="utf-8", newline="\n")
    return True


def descriptor_has_legacy_def(game_path: Path) -> bool:
    text = game_path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r'(?:entityclasstype|entityClassType)\s*=\s*"([^"]*)"', text)
    if not match:
        return False
    return "def" in match.group(1).split()


FGD_KEY_WITH_DEFAULT_RE = re.compile(
    r'^\s*([A-Za-z0-9_\.\-]+)\(([^)]+)\)\s*:\s*"([^"]*)"\s*:\s*([^:]+?)\s*:\s*"([^"]*)"\s*$'
)
FGD_KEY_NO_DEFAULT_RE = re.compile(
    r'^\s*([A-Za-z0-9_\.\-]+)\(([^)]+)\)\s*:\s*"([^"]*)"\s*:\s*"([^"]*)"\s*$'
)
FGD_KEY_BARE_RE = re.compile(
    r'^\s*([A-Za-z0-9_\.\-]+)\(([^)]+)\)\s*:\s*"([^"]*)"\s*$'
)
FGD_KEY_CHOICES_WITH_DEFAULT_RE = re.compile(
    r'^\s*([A-Za-z0-9_\.\-]+)\((choices)\)\s*:\s*"([^"]*)"\s*:\s*([^:]+?)\s*:\s*"([^"]*)"\s*=\s*$'
)
FGD_KEY_CHOICES_NO_DEFAULT_RE = re.compile(
    r'^\s*([A-Za-z0-9_\.\-]+)\((choices)\)\s*:\s*"([^"]*)"\s*:\s*"([^"]*)"\s*=\s*$'
)

CANONICAL_FGD_TYPE_ALIASES = {
    "float": "real",
    "target_destination": "target",
    "target_source": "targetname",
    "target_name_or_class": "target",
    "pointentityclass": "target",
    "origin": "vector3",
    "vector": "vector3",
    "vecline": "vector3",
    "axis": "vector3",
    "material": "texture",
    "studio": "model",
    "sprite": "texture",
    "color1": "color",
    "color255": "color",
    "direction": "direction",
    "node_dest": "direction",
}

@dataclass
class FgdKeyDecl:
    indent: str
    key: str
    key_type: str
    display_name: str
    description: str
    default_value: str | None


def parse_fgd_key_decl(line: str) -> FgdKeyDecl | None:
    indent = re.match(r"^\s*", line).group(0)
    stripped = line.strip()

    choices_with_default = FGD_KEY_CHOICES_WITH_DEFAULT_RE.match(stripped)
    if choices_with_default:
        return FgdKeyDecl(
            indent=indent,
            key=choices_with_default.group(1),
            key_type=choices_with_default.group(2).strip().lower(),
            display_name=choices_with_default.group(3),
            default_value=choices_with_default.group(4).strip(),
            description=choices_with_default.group(5),
        )

    choices_no_default = FGD_KEY_CHOICES_NO_DEFAULT_RE.match(stripped)
    if choices_no_default:
        return FgdKeyDecl(
            indent=indent,
            key=choices_no_default.group(1),
            key_type=choices_no_default.group(2).strip().lower(),
            display_name=choices_no_default.group(3),
            default_value=None,
            description=choices_no_default.group(4),
        )

    with_default = FGD_KEY_WITH_DEFAULT_RE.match(stripped)
    if with_default:
        return FgdKeyDecl(
            indent=indent,
            key=with_default.group(1),
            key_type=with_default.group(2).strip().lower(),
            display_name=with_default.group(3),
            default_value=with_default.group(4).strip(),
            description=with_default.group(5),
        )

    no_default = FGD_KEY_NO_DEFAULT_RE.match(stripped)
    if no_default:
        return FgdKeyDecl(
            indent=indent,
            key=no_default.group(1),
            key_type=no_default.group(2).strip().lower(),
            display_name=no_default.group(3),
            default_value=None,
            description=no_default.group(4),
        )

    bare = FGD_KEY_BARE_RE.match(stripped)
    if bare:
        return FgdKeyDecl(
            indent=indent,
            key=bare.group(1),
            key_type=bare.group(2).strip().lower(),
            display_name=bare.group(3),
            default_value=None,
            description="",
        )
    return None


def _append_fgd_key(lines: list[str], key_decl: FgdKeyDecl, choices: list[tuple[str, str]] | None = None) -> None:
    display = sanitize_text(key_decl.display_name)
    description = sanitize_text(key_decl.description)
    if choices:
        default_value = key_decl.default_value.strip() if key_decl.default_value else choices[0][0]
        lines.append(
            f'{key_decl.indent}{key_decl.key}(choices) : "{display}" : {default_value} : "{description}" ='
        )
        lines.append(f"{key_decl.indent}[")
        for value, label in choices:
            lines.append(f'{key_decl.indent}    {sanitize_text(value)} : "{sanitize_text(label)}"')
        lines.append(f"{key_decl.indent}]")
        return

    if key_decl.default_value is not None and key_decl.default_value.strip():
        lines.append(
            f'{key_decl.indent}{key_decl.key}({key_decl.key_type}) : "{display}" : {key_decl.default_value.strip()} : "{description}"'
        )
        return
    if not description:
        lines.append(f'{key_decl.indent}{key_decl.key}({key_decl.key_type}) : "{display}"')
        return
    lines.append(f'{key_decl.indent}{key_decl.key}({key_decl.key_type}) : "{display}" : "{description}"')


def _should_promote_numeric_choices(previous: FgdKeyDecl, numeric_options: list[FgdKeyDecl]) -> bool:
    if len(numeric_options) < 2:
        return False
    if previous.key_type in {"choices", "flags", "flag"}:
        return False

    values: list[int] = []
    for item in numeric_options:
        if not re.fullmatch(r"-?\d+", item.key):
            return False
        values.append(int(item.key))

    ordered = sorted(set(values))
    if len(ordered) != len(values):
        return False
    if ordered[0] not in {0, 1}:
        return False
    if any((b - a) != 1 for a, b in zip(ordered, ordered[1:])):
        return False

    key_name = previous.key.lower()
    text = f"{key_name} {previous.display_name.lower()} {previous.description.lower()}"
    hints = ("gametype", "mode", "color", "set to", "select", "choose", "option", "value")
    if any(token in text for token in hints):
        return True
    semantic_key_hints = ("sound", "style", "type", "count")
    return any(token in key_name for token in semantic_key_hints)


def _parse_choice_block(lines: list[str], start_index: int) -> tuple[list[tuple[str, str]] | None, int]:
    if start_index >= len(lines) or lines[start_index].strip() != "[":
        return None, start_index

    choices: list[tuple[str, str]] = []
    idx = start_index + 1
    while idx < len(lines):
        stripped = lines[idx].strip()
        if stripped == "]":
            if not choices:
                return None, idx
            return choices, idx
        option = re.match(r'^(-?\d+)\s*:\s*"([^"]*)"\s*$', stripped)
        if not option:
            return None, start_index
        choices.append((option.group(1), option.group(2)))
        idx += 1
    return None, start_index


def refine_fgd_file(path: Path) -> bool:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    changed = False
    refined: list[str] = []

    i = 0
    while i < len(lines):
        line = lines[i]
        key_decl = parse_fgd_key_decl(line)
        if key_decl is None:
            refined.append(line)
            i += 1
            continue

        if key_decl.key.lower() == "spawnflags" or key_decl.key_type in {"flags", "flag"}:
            refined.append(line)
            i += 1
            continue

        if re.fullmatch(r"-?\d+", key_decl.key):
            # Numeric key names are usually pseudo-choices from legacy defs; keep
            # them stable unless attached to a preceding key and promoted there.
            if key_decl.key_type == "choices":
                key_decl.key_type = "string"
                key_decl.default_value = None
                rebuilt = []
                _append_fgd_key(rebuilt, key_decl)
                refined.append(rebuilt[0])
                changed = True
                # Drop the synthetic choices block if present.
                if i + 1 < len(lines) and lines[i + 1].strip() == "[":
                    j = i + 2
                    while j < len(lines):
                        if lines[j].strip() == "]":
                            break
                        j += 1
                    i = min(j + 1, len(lines))
                else:
                    i += 1
                continue

            refined.append(line)
            i += 1
            continue

        if key_decl.key_type == "choices":
            refined.append(line)
            i += 1
            continue

        if should_skip_key(key_decl.key, key_decl.description):
            changed = True
            i += 1
            continue

        existing_choices, existing_choices_end = _parse_choice_block(lines, i + 1)
        if existing_choices:
            key_decl.key_type = "choices"
            if key_decl.default_value is None or not key_decl.default_value.strip():
                key_decl.default_value = existing_choices[0][0]
            _append_fgd_key(refined, key_decl, existing_choices)
            changed = True
            i = existing_choices_end + 1
            continue

        # Fold emitted numeric pseudo-keys (0/1/2/...) into a proper choice block.
        numeric_options: list[FgdKeyDecl] = []
        j = i + 1
        while j < len(lines):
            candidate = parse_fgd_key_decl(lines[j])
            if candidate is None:
                break
            if candidate.indent != key_decl.indent:
                break
            if candidate.key_type in {"flags", "flag"}:
                break
            if not re.fullmatch(r"-?\d+", candidate.key):
                break
            numeric_options.append(candidate)
            j += 1

        if numeric_options and _should_promote_numeric_choices(key_decl, numeric_options):
            choice_items = [(item.key, item.description or item.display_name) for item in numeric_options]
            key_decl.key_type = "choices"
            if key_decl.default_value is None or not key_decl.default_value.strip():
                key_decl.default_value = choice_items[0][0]
            _append_fgd_key(refined, key_decl, choice_items)
            changed = True
            i = j
            continue

        original_type = key_decl.key_type
        canonical_type = CANONICAL_FGD_TYPE_ALIASES.get(original_type, original_type)
        if canonical_type != original_type:
            key_decl.key_type = canonical_type
            changed = True

        inline_choices = extract_choices(key_decl.description)
        default_value = key_decl.default_value.strip() if key_decl.default_value else None
        inferred = infer_key_type(key_decl.key, key_decl.description, inline_choices, default_value)
        named_inferred = infer_key_type_by_name(key_decl.key, key_decl.description)

        new_type = key_decl.key_type
        if new_type in {"string", "integer", "real"}:
            if inferred != "string":
                new_type = inferred
        elif named_inferred and named_inferred != new_type:
            new_type = named_inferred
        elif inferred == "choices" and inline_choices:
            new_type = "choices"

        if new_type != key_decl.key_type:
            key_decl.key_type = new_type
            changed = True

        if key_decl.key_type == "choices" and inline_choices:
            _append_fgd_key(refined, key_decl, inline_choices)
            changed = True
        else:
            rebuilt = []
            _append_fgd_key(rebuilt, key_decl)
            rebuilt_line = rebuilt[0]
            if rebuilt_line != line:
                changed = True
            refined.append(rebuilt_line)
        i += 1

    if changed:
        path.write_text("\n".join(refined) + "\n", encoding="utf-8", newline="\n")
    return changed


def refine_existing_fgds(root: Path) -> int:
    refined = 0
    for fgd_path in sorted(root.rglob("*.fgd")):
        if refine_fgd_file(fgd_path):
            print(f"Refined {fgd_path}")
            refined += 1
    return refined


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default="games/VibePack",
        help="Root directory to scan (expects game descriptors under <root>/games)",
    )
    parser.add_argument(
        "--remove-legacy-def-files",
        action="store_true",
        help="Remove converted .def files after generating .fgd output",
    )
    parser.add_argument(
        "--fail-on-legacy-def",
        action="store_true",
        help="Return non-zero if any .def files or descriptor def tokens remain after conversion",
    )
    parser.add_argument(
        "--refine-existing-fgd",
        action="store_true",
        help="Retype and clean existing .fgd key declarations using current heuristics",
    )
    args = parser.parse_args()

    root = Path(args.root)
    game_descriptors = root / "games"
    if not root.is_dir():
        print(f"Root not found: {root}")
        return 1
    if not game_descriptors.is_dir():
        print(f"Descriptor directory not found: {game_descriptors}")
        return 1

    def_files = sorted(root.rglob("*.def"))
    if not def_files:
        print(f"No .def files found under {root}")

    converted = 0
    skipped: list[Path] = []
    for def_path in def_files:
        text = def_path.read_text(encoding="utf-8", errors="replace")
        entities = []
        for block_match in QUAKED_BLOCK_RE.finditer(text):
            entity = parse_quaked_block(block_match.group(1))
            if entity:
                entities.append(entity)
        if not entities:
            skipped.append(def_path)
            continue

        output_path = write_fgd(def_path, entities)
        print(f"Wrote {output_path}")
        converted += 1
        if args.remove_legacy_def_files:
            def_path.unlink()
            print(f"Removed legacy {def_path}")

    if skipped:
        print("Skipped .def files (no QUAKED blocks found):")
        for path in skipped:
            print(f"  - {path}")

    updated_games = 0
    for game_path in sorted(game_descriptors.glob("*.game")):
        if update_game_entityclasstype(game_path):
            print(f"Updated {game_path}")
            updated_games += 1

    print(f"Converted files: {converted}")
    print(f"Descriptors updated: {updated_games}")

    refined_fgd = 0
    if args.refine_existing_fgd:
        refined_fgd = refine_existing_fgds(root)
        print(f"FGD files refined: {refined_fgd}")

    if args.fail_on_legacy_def:
        remaining_defs = sorted(root.rglob("*.def"))
        legacy_descriptors = [path for path in sorted(game_descriptors.glob("*.game")) if descriptor_has_legacy_def(path)]
        if remaining_defs or legacy_descriptors:
            if remaining_defs:
                print("Legacy .def files remain:")
                for path in remaining_defs:
                    print(f"  - {path}")
            if legacy_descriptors:
                print("Descriptors still reference def entityclasstype:")
                for path in legacy_descriptors:
                    print(f"  - {path}")
            return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
