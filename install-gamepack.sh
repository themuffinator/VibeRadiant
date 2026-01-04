#!/bin/sh

# installs a game pack
# Usage:
#   install-gamepack.sh gamepack installdir

set -ex

: ${CP:=cp}
: ${CP_R:=cp -r}

pack=$1
dest=$2

# Some per-game workaround for malformed gamepack
case $pack in
	*/JediAcademyPack)
		pack="$pack/Tools"
	;;
	*/PreyPack|*/Quake3Pack)
		pack="$pack/tools"
	;;
	*/WolfPack)
		pack="$pack/bin"
	;;
	*/SmokinGunsPack|*/UnvanquishedPack)
		pack="$pack/build/netradiant"
	;;
	*/WoPPack)
		pack="$pack/netradiant"
	;;
esac

for GAMEFILE in "$pack/games"/*.game; do
	if [ x"$GAMEFILE" != x"$pack/games/*.game" ]; then
		$CP "$GAMEFILE" "$dest/games/"
	fi
done
for GAMEDIR in "$pack"/*.game; do
	if [ x"$GAMEDIR" != x"$pack/*.game" ]; then
		$CP_R "$GAMEDIR" "$dest/"

		# Auto-convert .def to .fgd
		TARGET_DIR="$dest/$(basename "$GAMEDIR")"
		CONVERTER="$(dirname "$0")/tools/def2fgd.py"

		if [ -f "$CONVERTER" ] && command -v python3 >/dev/null 2>&1; then
			find "$TARGET_DIR" -name "*.def" | while read -r DEF_FILE; do
				# Check if it looks like an entity definition
				if grep -q "QUAKED" "$DEF_FILE"; then
					FGD_FILE="${DEF_FILE%.def}.fgd"
					echo "Converting $DEF_FILE to $FGD_FILE"
					python3 "$CONVERTER" "$DEF_FILE" "$FGD_FILE" || echo "Conversion failed for $DEF_FILE"
				fi
			done
		fi
	fi
done
