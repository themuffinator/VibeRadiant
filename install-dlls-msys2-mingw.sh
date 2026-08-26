#!/bin/sh

set -e

: ${PYTHON:=python3}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
INSTALLDIR=${1:-${INSTALLDIR:-"$(pwd)/install"}}
MINGW_RUNTIME_PREFIX=${2:-${MINGW_PREFIX:-}}
QT_RUNTIME_MAJOR=${3:-${QT_MAJOR:-auto}}

resolve_python() {
	if command -v "$PYTHON" >/dev/null 2>&1; then
		printf '%s\n' "$PYTHON"
		return 0
	fi
	if command -v python3 >/dev/null 2>&1; then
		printf '%s\n' python3
		return 0
	fi
	if command -v python >/dev/null 2>&1; then
		printf '%s\n' python
		return 0
	fi
	return 1
}

python_cmd=$(resolve_python) || {
	printf 'Python interpreter not found; cannot stage MSYS2 runtime DLLs.\n' >&2
	exit 1
}

if [ -n "$MINGW_RUNTIME_PREFIX" ]; then
	exec "$python_cmd" "$SCRIPT_DIR/tools/stage_msys2_runtime.py" \
		--install-dir "$INSTALLDIR" \
		--mingw-prefix "$MINGW_RUNTIME_PREFIX" \
		--qt-major "$QT_RUNTIME_MAJOR"
fi

exec "$python_cmd" "$SCRIPT_DIR/tools/stage_msys2_runtime.py" \
	--install-dir "$INSTALLDIR" \
	--qt-major "$QT_RUNTIME_MAJOR"
