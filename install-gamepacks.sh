#!/bin/sh

: ${ECHO:=echo}
: ${SH:=sh}
: ${CP:=cp}
: ${CP_R:=cp -r}
: ${RM:=rm}
: ${RM_R:=rm -r}
: ${GAMEPACK_SOURCE:=VibePack}
: ${PYTHON:=python3}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)

dest=$1
if [ -z "$dest" ]; then
	$ECHO "Usage: $0 <destination-gamepacks-dir>"
	exit 1
fi

case "$DOWNLOAD_GAMEPACKS" in
	yes)
		LICENSEFILTER=GPL BATCH=1 $SH "$SCRIPT_DIR/download-gamepacks.sh"
		;;
	allinone)
		LICENSEFILTER=allinone BATCH=1 $SH "$SCRIPT_DIR/download-gamepacks.sh"
		;;
	all)
		BATCH=1 $SH "$SCRIPT_DIR/download-gamepacks.sh"
		;;
	*)
		;;
esac

set -e

install_one_pack() {
	packdir=$1
	if [ ! -d "$packdir" ]; then
		return 1
	fi
	$SH "$SCRIPT_DIR/install-gamepack.sh" "$packdir" "$dest"
	return 0
}

prepare_destination() {
	mkdir -p "$dest" "$dest/games"
	for GAMEFILE in "$dest"/games/*.game; do
		if [ -f "$GAMEFILE" ]; then
			$RM -f "$GAMEFILE"
		fi
	done
	for GAMEDIR in "$dest"/*.game; do
		if [ -d "$GAMEDIR" ]; then
			$RM_R "$GAMEDIR"
		fi
	done
}

resolve_python() {
	if command -v "$PYTHON" >/dev/null 2>&1; then
		$ECHO "$PYTHON"
		return 0
	fi
	if command -v python3 >/dev/null 2>&1; then
		$ECHO python3
		return 0
	fi
	if command -v python >/dev/null 2>&1; then
		$ECHO python
		return 0
	fi
	return 1
}

normalize_entity_definitions() {
	python_cmd=$(resolve_python) || {
		$ECHO "Python interpreter not found; cannot normalize legacy .def entity definitions."
		return 1
	}

	"$python_cmd" "$SCRIPT_DIR/tools/convert_entities_def_to_fgd.py" \
		--root "$dest" \
		--remove-legacy-def-files \
		--refine-existing-fgd \
		--fail-on-legacy-def
}

print_missing_packs_error() {
	$ECHO "Game packs not found, please run"
	$ECHO "  $SCRIPT_DIR/download-gamepacks.sh"
	$ECHO "and then try again!"
}

prepare_destination

case "$GAMEPACK_SOURCE" in
	auto)
		if install_one_pack "games/VibePack"; then
			$ECHO "Using gamepack source: games/VibePack"
		elif install_one_pack "games/NRCPack"; then
			$ECHO "Using gamepack source: games/NRCPack"
		else
			print_missing_packs_error
			exit 1
		fi
		;;
	all)
		$ECHO "GAMEPACK_SOURCE=all is no longer supported."
		$ECHO "Install from a single canonical source (VibePack or auto fallback)."
		exit 1
		;;
	*)
		if install_one_pack "games/$GAMEPACK_SOURCE"; then
			$ECHO "Using gamepack source: games/$GAMEPACK_SOURCE"
		else
			$ECHO "Configured GAMEPACK_SOURCE '$GAMEPACK_SOURCE' was not found."
			print_missing_packs_error
			exit 1
		fi
		;;
esac

normalize_entity_definitions
