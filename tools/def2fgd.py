#!/usr/bin/env python3
import sys
import re
import os

def parse_def(content):
    entities = []
    # Split by /*QUAKED
    parts = content.split('/*QUAKED ')

    for part in parts[1:]:
        end_idx = part.find('*/')
        if end_idx == -1:
            continue

        block = part[:end_idx]
        lines = block.splitlines()

        if not lines:
            continue

        header = lines[0].strip()
        # Parse header
        # classname (r g b) (min x y z) (max x y z) FLAG1 FLAG2 ...

        # Regex for header
        # group 1: classname (allow anything except whitespace and paren)
        # group 2: color
        # group 3: mins (optional)
        # group 4: maxs (optional)
        # group 5: is_brush (optional '?' marker)
        # group 6: flags (rest)

        # We allow 0 or more spaces between classname and color block, as some .def files might lack space
        match = re.match(r'^([^\s\(]+)\s*\(([^)]+)\)\s*(?:\(([^)]+)\)\s*\(([^)]+)\)|(\?))?\s*(.*)$', header)

        if not match:
            print(f"Skipping malformed header: {header}")
            continue

        classname = match.group(1)
        color = match.group(2)
        mins = match.group(3)
        maxs = match.group(4)
        is_brush = match.group(5) == '?'
        flags_str = match.group(6)

        flags_list = flags_str.split()

        description = []
        keys = {}
        spawnflags_desc = {}

        current_section = "desc"

        for line in lines[1:]:
            line = line.strip()
            if not line:
                continue

            if line.startswith('--------'):
                if 'KEYS' in line:
                    current_section = "keys"
                elif 'SPAWNFLAGS' in line:
                    current_section = "spawnflags"
                else:
                    current_section = "other"
                continue

            if current_section == "desc":
                description.append(line)
            elif current_section == "keys":
                # key : description
                if ':' in line:
                    k, d = line.split(':', 1)
                    keys[k.strip()] = d.strip()
            elif current_section == "spawnflags":
                # FLAG : description
                if ':' in line:
                    f, d = line.split(':', 1)
                    # The description might contain &1 or similar, but we rely on header position
                    f_name = f.strip()
                    d_desc = d.strip()
                    # Remove &number from description if present
                    d_desc = re.sub(r'^&\d+\s+', '', d_desc)
                    spawnflags_desc[f_name] = d_desc

        entities.append({
            'classname': classname,
            'color': color,
            'mins': mins,
            'maxs': maxs,
            'is_brush': is_brush,
            'header_flags': flags_list,
            'description': "\n".join(description),
            'keys': keys,
            'spawnflags_desc': spawnflags_desc
        })

    return entities

def guess_key_type(key_name):
    key_name = key_name.lower()
    if key_name in ['target', 'killtarget', 'target2', 'target3', 'target4']:
        return 'target_destination'
    if key_name in ['targetname']:
        return 'target_source'
    if key_name in ['angle']:
        return 'angle'
    if key_name in ['angles']:
        return 'angle' # or string
    if key_name in ['model', 'model2']:
        return 'studio'
    if key_name in ['noise', 'noise_start', 'noise_stop', 'music', 'sound']:
        return 'sound'
    if key_name in ['color', '_color']:
        return 'color255'
    if key_name in ['origin']:
        return 'origin'
    if key_name in ['health', 'speed', 'wait', 'dmg', 'lip', 'height', 'light', 'count', 'mass', 'damage']:
        return 'integer' # or string/float, but integer covers most
    if key_name.startswith('not') or key_name in ['spawnflags']:
        return 'integer'
    return 'string'

def generate_fgd(entities):
    output = []

    # Check for Base Classes (not present in .def usually, but we can structure .fgd to use them if we wanted)
    # For now, flat structure.

    for ent in entities:
        classname = ent['classname']
        color_parts = ent['color'].split()
        # Ensure 3 components and integers 0-255. In def they are often 0.0-1.0 float.
        color_rgb = []
        for c in color_parts:
            try:
                val = float(c)
                if val <= 1.0:
                    val = int(val * 255)
                else:
                    val = int(val)
                color_rgb.append(str(val))
            except ValueError:
                color_rgb.append("255")

        while len(color_rgb) < 3:
            color_rgb.append("0")
        color_str = " ".join(color_rgb[:3])

        if ent['is_brush']:
            class_type = "@SolidClass"
            size_str = ""
        else:
            class_type = "@PointClass"
            if ent['mins'] and ent['maxs']:
                size_str = f" size({ent['mins']}, {ent['maxs']})"
            else:
                # Default size? or no size
                size_str = ""

        # Description might be multi-line, need to escape quotes?
        description = ent['description'].replace('"', "'")

        output.append(f'{class_type} color({color_str}){size_str} = {classname} : "{description}"')
        output.append("[")

        # Keys
        for key, desc in ent['keys'].items():
            k_type = guess_key_type(key)
            k_desc = desc.replace('"', "'")
            # Default value is not in .def usually
            default_val = ""
            output.append(f'\t{key}({k_type}) : "{k_desc}" : "{default_val}" : ""')

        # Spawnflags
        if ent['header_flags']:
            output.append("\tspawnflags(flags) =")
            output.append("\t[")
            for i, flag in enumerate(ent['header_flags']):
                if flag == '-' or flag == '?':
                    continue

                bit = 1 << i
                desc = ent['spawnflags_desc'].get(flag, "")
                desc = desc.replace('"', "'")
                # default 0
                output.append(f'\t\t{bit} : "{flag}" : 0 : "{desc}"')
            output.append("\t]")

        output.append("]")
        output.append("")

    return "\n".join(output)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: def2fgd.py <input.def> [output.fgd]")
        sys.exit(1)

    input_file = sys.argv[1]
    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        output_file = os.path.splitext(input_file)[0] + ".fgd"

    with open(input_file, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()

    entities = parse_def(content)
    fgd_content = generate_fgd(entities)

    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(fgd_content)

    print(f"Converted {len(entities)} entities to {output_file}")
