#!/bin/sh

: ${ECHO:=echo}
: ${SH:=sh}
: ${CP:=cp}
: ${CP_R:=cp -r}
: ${RM:=rm}
: ${RM_R:=rm -r}
: ${MV:=mv}
: ${GAMEPACK_SOURCE:=VibePack}
: ${PYTHON:=python3}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)

set -e

dest=$1
if [ -z "$dest" ]; then
	$ECHO "Usage: $0 <destination-gamepacks-dir>"
	exit 1
fi

if [ "$GAMEPACK_SOURCE" != "VibePack" ]; then
	$ECHO "GAMEPACK_SOURCE='$GAMEPACK_SOURCE' is no longer supported."
	$ECHO "Use the canonical source only: GAMEPACK_SOURCE=VibePack."
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
	normalize_root=$1
	python_cmd=$(resolve_python) || {
		$ECHO "Python interpreter not found; cannot normalize legacy .def entity definitions."
		return 1
	}

	"$python_cmd" "$SCRIPT_DIR/tools/convert_entities_def_to_fgd.py" \
		--root "$normalize_root" \
		--remove-legacy-def-files \
		--refine-existing-fgd \
		--fail-on-legacy-def
}

print_missing_packs_error() {
	$ECHO "Game packs not found, please run"
	$ECHO "  $SCRIPT_DIR/download-gamepacks.sh"
	$ECHO "and then try again!"
}

source_pack="$SCRIPT_DIR/games/VibePack"
if [ ! -d "$source_pack/games" ]; then
	print_missing_packs_error
	exit 1
fi

dest_parent=$(dirname "$dest")
mkdir -p "$dest_parent"
stage_dir=$(mktemp -d "$dest_parent/.gamepacks-stage.XXXXXX")
backup_dir=

cleanup_staging() {
	if [ -n "$stage_dir" ] && [ -d "$stage_dir" ]; then
		$RM_R "$stage_dir"
	fi
	if [ -n "$backup_dir" ] && [ -d "$backup_dir" ] && [ ! -e "$dest" ]; then
		$MV "$backup_dir" "$dest"
	fi
}
trap cleanup_staging EXIT
trap 'exit 1' HUP INT TERM

mkdir -p "$stage_dir/games"
$SH "$SCRIPT_DIR/install-gamepack.sh" "$source_pack" "$stage_dir"
$ECHO "Using gamepack source: games/VibePack"
normalize_entity_definitions "$stage_dir"

if [ -e "$dest" ]; then
	backup_dir="$dest.backup.$$"
	if [ -e "$backup_dir" ]; then
		$ECHO "Refusing to overwrite stale gamepack backup: $backup_dir"
		exit 1
	fi
	$MV "$dest" "$backup_dir"
fi

if ! $MV "$stage_dir" "$dest"; then
	$ECHO "Unable to activate staged gamepacks; restoring previous installation."
	exit 1
fi
stage_dir=

if [ -n "$backup_dir" ]; then
	$RM_R "$backup_dir"
	backup_dir=
fi
