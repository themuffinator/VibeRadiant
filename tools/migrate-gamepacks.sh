#!/bin/sh
set -e

# This script attempts to migrate entity.def files to .fgd files in the games/ directory.
# It uses tools/def2fgd.py.

if [ ! -d "games" ]; then
	echo "games/ directory not found. Please run download-gamepacks.sh first."
	exit 1
fi

TOOLS_DIR=$(dirname "$0")
CONVERTER="$TOOLS_DIR/def2fgd.py"

find games -name "*.def" | while read -r def_file; do
	fgd_file="${def_file%.def}.fgd"
	echo "Converting $def_file to $fgd_file..."
	python3 "$CONVERTER" "$def_file" "$fgd_file"
done

echo "Migration complete."
