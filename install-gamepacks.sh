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

python_cmd=$(resolve_python) || {
	$ECHO "Python interpreter not found; cannot install gamepacks."
	exit 1
}
state_tool="$SCRIPT_DIR/tools/gamepack_install_state.py"
if [ ! -f "$state_tool" ]; then
	$ECHO "Gamepack install-state helper not found: $state_tool"
	exit 1
fi
install_umask=$(umask)

gamepack_state() {
	state_mode=$1
	shift
	"$python_cmd" "$state_tool" "$state_mode" \
		--source "$source_pack" \
		--transform "$SCRIPT_DIR/install-gamepacks.sh" \
		--transform "$SCRIPT_DIR/install-gamepack.sh" \
		--transform "$SCRIPT_DIR/tools/convert_entities_def_to_fgd.py" \
		--transform "$state_tool" \
		--parameter "SH=$SH" \
		--parameter "CP=$CP" \
		--parameter "CP_R=$CP_R" \
		--parameter "RM=$RM" \
		--parameter "RM_R=$RM_R" \
		--parameter "MV=$MV" \
		--parameter "PYTHON_COMMAND=$python_cmd" \
		--parameter "GAMEPACK_SOURCE=$GAMEPACK_SOURCE" \
		--parameter "umask=$install_umask" \
		"$@"
}

if [ "${GAMEPACK_FORCE_REBUILD:-0}" != "1" ]; then
	set +e
	gamepack_state check --destination "$dest"
	state_status=$?
	set -e
	case $state_status in
		0)
			$ECHO "Gamepacks are unchanged; keeping existing installation."
			exit 0
			;;
		1)
			;;
		*)
			exit "$state_status"
			;;
	esac
fi
expected_input=$(gamepack_state input)

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
gamepack_state write --destination "$stage_dir" --expected-input "$expected_input"

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
