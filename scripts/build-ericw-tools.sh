#!/bin/sh

set -e

case "$0" in
	*/*) SCRIPT_PATH="$0" ;;
	*) SCRIPT_PATH="./$0" ;;
esac
SCRIPT_DIR=$(CDPATH= cd -- "${SCRIPT_PATH%/*}" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

MODE=install
SOURCE_DIR="$REPO_ROOT/tools/quake2/ericw-tools"
BUILD_DIR="$REPO_ROOT/build-ericw-tools"
DEST_DIR=
BUILD_TYPE=Release
EXECUTABLE_SUFFIX=auto
STAMP_FILE=

usage() {
	echo "Usage: $0 [--mode build|install|stage] [--source <dir>] [--build-dir <dir>] [--dest <dir>] [--build-type <type>]"
	echo "  --mode build    Configure/build vmt-bsp, vmt-light, vmt-vis"
	echo "  --mode install  Build and copy tools into <dest> as qbsp/light/vis (default)"
	echo "  --mode stage    Copy an existing build without rebuilding"
	echo "  --dest          Required when --mode install"
	echo "  --executable-suffix  Installed suffix without a leading dot (default: auto)"
	echo "  --stamp         Write a success stamp after --mode build"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--mode)
			MODE="$2"
			shift 2
			;;
		--source)
			SOURCE_DIR="$2"
			shift 2
			;;
		--build-dir)
			BUILD_DIR="$2"
			shift 2
			;;
		--dest)
			DEST_DIR="$2"
			shift 2
			;;
		--build-type)
			BUILD_TYPE="$2"
			shift 2
			;;
		--executable-suffix)
			EXECUTABLE_SUFFIX="$2"
			shift 2
			;;
		--stamp)
			STAMP_FILE="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown argument: $1"
			usage
			exit 1
			;;
	esac
done

if [ "$MODE" != "build" ] && [ "$MODE" != "install" ] && [ "$MODE" != "stage" ]; then
	echo "Invalid --mode '$MODE'"
	usage
	exit 1
fi

if [ ! -f "$SOURCE_DIR/CMakeLists.txt" ]; then
	echo "ericw-tools source not found: $SOURCE_DIR"
	exit 1
fi

if { [ "$MODE" = "install" ] || [ "$MODE" = "stage" ]; } && [ -z "$DEST_DIR" ]; then
	echo "--dest is required for --mode install"
	usage
	exit 1
fi

if [ "$MODE" != "stage" ]; then
	mkdir -p "$BUILD_DIR"
	CONFIG_STAMP="$BUILD_DIR/.viberadiant-ericw-config"
	CONFIG_SIGNATURE="source=$SOURCE_DIR
build_type=$BUILD_TYPE"

	CONFIGURE_BUILD=1
	if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ -f "$CONFIG_STAMP" ] &&
		[ "$(cat "$CONFIG_STAMP")" = "$CONFIG_SIGNATURE" ]; then
		CONFIGURE_BUILD=0
	fi

	if [ "$CONFIGURE_BUILD" -eq 1 ]; then
		cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
			-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
			-DDISABLE_TESTS=ON \
			-DDISABLE_DOCS=ON \
			-DENABLE_HUB=NO
		printf '%s\n' "$CONFIG_SIGNATURE" > "$CONFIG_STAMP"
	fi

	# CMake's generated build graph performs its own source dependency and
	# reconfigure checks. This makes an always-stale outer Meson target cheap
	# while still rebuilding when any vendored source changes.
	cmake --build "$BUILD_DIR" --target vmt-bsp vmt-light vmt-vis

	if [ -n "$STAMP_FILE" ]; then
		case "$STAMP_FILE" in
			*/*) mkdir -p "${STAMP_FILE%/*}" ;;
		esac
		printf 'built\n' > "$STAMP_FILE"
	fi

	if [ "$MODE" = "build" ]; then
		exit 0
	fi
fi

copy_if_exists() {
	src=$1
	dst=$2
	if [ -f "$src" ]; then
		cp -f "$src" "$dst"
		return 0
	fi
	return 1
}

SOURCE_EXE_SUFFIX=
if [ -f "$BUILD_DIR/src/qbsp/vmt-bsp.exe" ]; then
	SOURCE_EXE_SUFFIX=.exe
fi

if [ "$EXECUTABLE_SUFFIX" = "auto" ]; then
	OUTPUT_EXE_SUFFIX="$SOURCE_EXE_SUFFIX"
else
	OUTPUT_EXE_SUFFIX=.$EXECUTABLE_SUFFIX
fi

mkdir -p "$DEST_DIR"

if ! copy_if_exists "$BUILD_DIR/src/qbsp/vmt-bsp$SOURCE_EXE_SUFFIX" "$DEST_DIR/qbsp$OUTPUT_EXE_SUFFIX"; then
	echo "Missing built binary: $BUILD_DIR/src/qbsp/vmt-bsp$SOURCE_EXE_SUFFIX"
	exit 1
fi
if ! copy_if_exists "$BUILD_DIR/src/light/vmt-light$SOURCE_EXE_SUFFIX" "$DEST_DIR/light$OUTPUT_EXE_SUFFIX"; then
	echo "Missing built binary: $BUILD_DIR/src/light/vmt-light$SOURCE_EXE_SUFFIX"
	exit 1
fi
if ! copy_if_exists "$BUILD_DIR/src/vis/vmt-vis$SOURCE_EXE_SUFFIX" "$DEST_DIR/vis$OUTPUT_EXE_SUFFIX"; then
	echo "Missing built binary: $BUILD_DIR/src/vis/vmt-vis$SOURCE_EXE_SUFFIX"
	exit 1
fi

for tool_dir in "$BUILD_DIR/src/qbsp" "$BUILD_DIR/src/light" "$BUILD_DIR/src/vis"; do
	for f in "$tool_dir"/*.dll "$tool_dir"/*.so "$tool_dir"/*.so.* "$tool_dir"/*.dylib "$tool_dir"/LICENSE*.txt; do
		if [ -f "$f" ]; then
			cp -f "$f" "$DEST_DIR/"
		fi
	done
done

cp -f "$SOURCE_DIR/COPYING" "$DEST_DIR/LICENSE.txt"
cp -f "$SOURCE_DIR/THIRD_PARTY_NOTICES.md" "$DEST_DIR/THIRD_PARTY_NOTICES.md"
cp -f "$SOURCE_DIR/VIBERADIANT_VENDOR.md" "$DEST_DIR/VIBERADIANT_VENDOR.md"
cp -f "$SOURCE_DIR/extern/fmt/LICENSE" "$DEST_DIR/LICENSE-fmt.txt"
cp -f "$SOURCE_DIR/extern/jsoncpp/LICENSE" "$DEST_DIR/LICENSE-jsoncpp.txt"
cp -f "$SOURCE_DIR/extern/nanobench/LICENSE" "$DEST_DIR/LICENSE-nanobench.txt"
cp -f "$SOURCE_DIR/extern/pareto/LICENSE" "$DEST_DIR/LICENSE-pareto.txt"

if [ -n "$OUTPUT_EXE_SUFFIX" ] && [ -z "$SOURCE_EXE_SUFFIX" ]; then
	for tool in qbsp light vis; do
		(
			cd "$DEST_DIR"
			ln -snf "$tool$OUTPUT_EXE_SUFFIX" "$tool"
		) || cp -f "$DEST_DIR/$tool$OUTPUT_EXE_SUFFIX" "$DEST_DIR/$tool"
	done
fi

if [ "$OUTPUT_EXE_SUFFIX" != "" ]; then
	echo "Installed ericw tools to $DEST_DIR (suffix: $OUTPUT_EXE_SUFFIX)."
else
	echo "Installed ericw tools to $DEST_DIR."
fi
