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
GENERATOR=${CMAKE_GENERATOR:-}
CMAKE_TOOL=cmake
EXECUTABLE_SUFFIX=auto
STAMP_FILE=
DEPFILE=
DEPFILE_HELPER="$SCRIPT_DIR/emit-ericw-depfile.py"
PYTHON_COMMAND=python3

usage() {
	echo "Usage: $0 [--mode build|install|stage] [--source <dir>] [--build-dir <dir>] [--dest <dir>] [--build-type <type>]"
	echo "  --mode build    Configure/build vmt-bsp, vmt-light, vmt-vis"
	echo "  --mode install  Build and copy tools into <dest> as qbsp/light/vis (default)"
	echo "  --mode stage    Copy an existing build without rebuilding"
	echo "  --dest          Required when --mode install or --mode stage"
	echo "  --generator     CMake generator (defaults to CMAKE_GENERATOR or CMake's default)"
	echo "  --cmake         CMake executable (default: cmake from PATH)"
	echo "  --executable-suffix  Installed suffix without a leading dot (default: auto)"
	echo "  --stamp         Write a success stamp after --mode build"
	echo "  --depfile       Write nested build dependencies for an outer build system"
	echo "  --depfile-helper  Dependency scanner (default: scripts/emit-ericw-depfile.py)"
	echo "  --python        Python interpreter used by the dependency scanner"
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
		--generator)
			GENERATOR="$2"
			shift 2
			;;
		--cmake)
			CMAKE_TOOL="$2"
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
		--depfile)
			DEPFILE="$2"
			shift 2
			;;
		--depfile-helper)
			DEPFILE_HELPER="$2"
			shift 2
			;;
		--python)
			PYTHON_COMMAND="$2"
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

SOURCE_DIR=$(CDPATH= cd -- "$SOURCE_DIR" && pwd)
mkdir -p "$BUILD_DIR"
BUILD_DIR=$(CDPATH= cd -- "$BUILD_DIR" && pwd)

if { [ "$MODE" = "install" ] || [ "$MODE" = "stage" ]; } && [ -z "$DEST_DIR" ]; then
	echo "--dest is required for --mode install or --mode stage"
	usage
	exit 1
fi

if [ -n "$DEPFILE" ] && [ ! -f "$DEPFILE_HELPER" ]; then
	echo "ericw-tools depfile helper not found: $DEPFILE_HELPER"
	exit 1
fi
if [ -n "$DEPFILE" ] && [ -z "$STAMP_FILE" ]; then
	echo "--depfile requires --stamp so the dependency rule has an output"
	exit 1
fi

CONFIGURE_GENERATOR=$GENERATOR
if [ -n "$GENERATOR" ] && [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
	CACHED_GENERATOR=$(sed -n 's/^CMAKE_GENERATOR:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt" | sed -n '1p')
	if [ -n "$CACHED_GENERATOR" ] && [ "$CACHED_GENERATOR" != "$GENERATOR" ]; then
		# Preserve pre-existing build trees rather than asking CMake to switch a
		# generator in place. Fresh Meson build trees still use Ninja explicitly.
		CONFIGURE_GENERATOR=
	fi
fi

cmake_cache_value() {
	cache_key=$1
	sed -n "s/^${cache_key}:[^=]*=//p" "$BUILD_DIR/CMakeCache.txt" | sed -n '1p'
}

tool_version_line() {
	tool_path=$1
	if [ -n "$tool_path" ] && [ -x "$tool_path" ]; then
		"$tool_path" --version 2>/dev/null | sed -n '1p'
	fi
}

build_config_signature() {
	config_generator=$(cmake_cache_value CMAKE_GENERATOR)
	config_cmake=$(cmake_cache_value CMAKE_COMMAND)
	config_c_compiler=$(cmake_cache_value CMAKE_C_COMPILER)
	config_cxx_compiler=$(cmake_cache_value CMAKE_CXX_COMPILER)
	config_make_program=$(cmake_cache_value CMAKE_MAKE_PROGRAM)
	printf '%s\n' \
		"source=$SOURCE_DIR" \
		"build_type=$BUILD_TYPE" \
		"disable_tests=ON" \
		"disable_docs=ON" \
		"enable_hub=NO" \
		"msystem=${MSYSTEM:-}" \
		"requested_generator=$GENERATOR" \
		"requested_cmake=$CMAKE_TOOL" \
		"requested_cmake_version=$(tool_version_line "$CMAKE_TOOL")" \
		"generator=$config_generator" \
		"cmake=$config_cmake" \
		"cmake_version=$(tool_version_line "$config_cmake")" \
		"c_compiler=$config_c_compiler" \
		"c_compiler_version=$(tool_version_line "$config_c_compiler")" \
		"cxx_compiler=$config_cxx_compiler" \
		"cxx_compiler_version=$(tool_version_line "$config_cxx_compiler")" \
		"make_program=$config_make_program" \
		"make_program_version=$(tool_version_line "$config_make_program")"
}

write_text_atomic() {
	atomic_destination=$1
	atomic_contents=$2
	case "$atomic_destination" in
		*/*) mkdir -p "${atomic_destination%/*}" ;;
	esac
	atomic_temporary=$(mktemp "${atomic_destination}.tmp.XXXXXX")
	trap 'rm -f -- "$atomic_temporary"' 0
	trap 'rm -f -- "$atomic_temporary"; exit 1' HUP INT TERM
	printf '%s\n' "$atomic_contents" > "$atomic_temporary"
	mv -f -- "$atomic_temporary" "$atomic_destination"
	atomic_temporary=
	trap - 0 HUP INT TERM
}

generator_build_file_exists() {
	case "$(cmake_cache_value CMAKE_GENERATOR)" in
		Ninja*) [ -f "$BUILD_DIR/build.ninja" ] ;;
		*Makefiles*) [ -f "$BUILD_DIR/Makefile" ] ;;
		*) return 0 ;;
	esac
}

if [ "$MODE" != "stage" ]; then
	CONFIG_STAMP="$BUILD_DIR/.viberadiant-ericw-config"

	CONFIGURE_BUILD=1
	if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ -f "$CONFIG_STAMP" ] && generator_build_file_exists; then
		CONFIG_SIGNATURE=$(build_config_signature)
		if [ "$(cat "$CONFIG_STAMP")" = "$CONFIG_SIGNATURE" ]; then
			CONFIGURE_BUILD=0
		fi
	fi

	if [ "$CONFIGURE_BUILD" -eq 1 ]; then
		if [ -n "$CONFIGURE_GENERATOR" ]; then
			"$CMAKE_TOOL" -S "$SOURCE_DIR" -B "$BUILD_DIR" -G "$CONFIGURE_GENERATOR" \
				-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
				-DDISABLE_TESTS=ON \
				-DDISABLE_DOCS=ON \
				-DENABLE_HUB=NO
		else
			if [ -n "$GENERATOR" ] && [ -n "${CACHED_GENERATOR:-}" ] && [ "$CACHED_GENERATOR" != "$GENERATOR" ]; then
				echo "Keeping existing CMake generator '$CACHED_GENERATOR' (requested '$GENERATOR')."
			fi
			"$CMAKE_TOOL" -S "$SOURCE_DIR" -B "$BUILD_DIR" \
				-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
				-DDISABLE_TESTS=ON \
				-DDISABLE_DOCS=ON \
				-DENABLE_HUB=NO
		fi
		CONFIG_SIGNATURE=$(build_config_signature)
		write_text_atomic "$CONFIG_STAMP" "$CONFIG_SIGNATURE"
	fi

	# CMake's generated graph resolves the exact nested rebuild work. The outer
	# depfile keeps Meson dormant until an input, build tool, or output changes.
	"$CMAKE_TOOL" --build "$BUILD_DIR" --target vmt-bsp vmt-light vmt-vis

	BUILT_EXE_SUFFIX=
	if [ -f "$BUILD_DIR/src/qbsp/vmt-bsp.exe" ]; then
		BUILT_EXE_SUFFIX=.exe
	elif [ ! -f "$BUILD_DIR/src/qbsp/vmt-bsp" ]; then
		echo "Missing built binary: $BUILD_DIR/src/qbsp/vmt-bsp"
		exit 1
	fi

	if [ -n "$DEPFILE" ]; then
		"$PYTHON_COMMAND" "$DEPFILE_HELPER" \
			--source-dir "$SOURCE_DIR" \
			--build-dir "$BUILD_DIR" \
			--depfile "$DEPFILE" \
			--target "$STAMP_FILE" \
			--explicit "$SCRIPT_PATH" \
			--explicit "$DEPFILE_HELPER" \
			--output "src/qbsp/vmt-bsp$BUILT_EXE_SUFFIX" \
			--output "src/light/vmt-light$BUILT_EXE_SUFFIX" \
			--output "src/vis/vmt-vis$BUILT_EXE_SUFFIX"
	fi

	if [ -n "$STAMP_FILE" ]; then
		write_text_atomic "$STAMP_FILE" "built"
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
