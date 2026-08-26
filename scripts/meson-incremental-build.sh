#!/usr/bin/env bash

set -euo pipefail

usage() {
	cat >&2 <<'EOF'
usage: meson-incremental-build.sh --build-dir DIR --profile PROFILE
       --qt-major MAJOR --jobs COUNT --prefix DIR --msys2-root DIR
       --subsystem NAME [--install-runtime]
EOF
	exit 2
}

build_dir=
build_profile=
qt_major=
jobs=
install_prefix=
msys2_root=
subsystem=
install_runtime=false

while [[ $# -gt 0 ]]; do
	case "$1" in
		--build-dir)
			[[ $# -ge 2 ]] || usage
			build_dir=$2
			shift 2
			;;
		--profile)
			[[ $# -ge 2 ]] || usage
			build_profile=$2
			shift 2
			;;
		--qt-major)
			[[ $# -ge 2 ]] || usage
			qt_major=$2
			shift 2
			;;
		--jobs)
			[[ $# -ge 2 ]] || usage
			jobs=$2
			shift 2
			;;
		--prefix)
			[[ $# -ge 2 ]] || usage
			install_prefix=$2
			shift 2
			;;
		--msys2-root)
			[[ $# -ge 2 ]] || usage
			msys2_root=$2
			shift 2
			;;
		--subsystem)
			[[ $# -ge 2 ]] || usage
			subsystem=$2
			shift 2
			;;
		--install-runtime)
			install_runtime=true
			shift
			;;
		*)
			usage
			;;
	esac
done

[[ -n "$build_dir" && "$build_dir" != -* ]] || usage
case "$build_profile" in
	debug|release) ;;
	*) usage ;;
esac
case "$qt_major" in
	5|6) ;;
	*) usage ;;
esac
[[ "$jobs" =~ ^[0-9]+$ ]] || usage
[[ -n "$install_prefix" ]] || usage
[[ -n "$msys2_root" ]] || usage
[[ "$subsystem" =~ ^[[:alnum:]_-]+$ ]] || usage

expected_prefix=$install_prefix
if [[ "$expected_prefix" =~ ^/([[:alpha:]])/(.*)$ ]]; then
	expected_prefix="${BASH_REMATCH[1]^^}:/${BASH_REMATCH[2]}"
fi

source_dir=$(pwd -P)
msys2_root=${msys2_root//\\//}
if [[ "$msys2_root" =~ ^([[:alpha:]]:)/+(.*)$ ]]; then
	msys2_root="${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"
elif [[ "$msys2_root" =~ ^/([[:alpha:]])/(.*)$ ]]; then
	msys2_root="${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
fi
msys2_root=${msys2_root%/}
msys2_root=${msys2_root,,}
subsystem=${subsystem,,}
expected_context="source=$source_dir"$'\n'"msys2_root=$msys2_root"$'\n'"subsystem=$subsystem"

coredata="$build_dir/meson-private/coredata.dat"
command_line="$build_dir/meson-private/cmd_line.txt"
context_file="$build_dir/meson-private/viberadiant-build-context"

context_matches() {
	[[ -f "$context_file" ]] && [[ "$(<"$context_file")" == "$expected_context" ]]
}

ninja_escape() {
	local input=$1 output= character
	while [[ -n "$input" ]]; do
		character=${input:0:1}
		input=${input:1}
		case "$character" in
			'$') output+='$$' ;;
			' ') output+='$ ' ;;
			*) output+=$character ;;
		esac
	done
	printf '%s' "$output"
}

legacy_context_matches() {
	[[ -f "$build_dir/build.ninja" ]] || return 1
	local source_windows source_forward meson_windows meson_forward
	source_windows=$(cygpath -aw "$source_dir") || return 1
	source_forward=$(cygpath -am "$source_dir") || return 1
	meson_windows=$(cygpath -aw "$msys2_root/$subsystem/bin/meson") || return 1
	meson_forward=$(cygpath -am "$msys2_root/$subsystem/bin/meson") || return 1
	meson_windows=${meson_windows%.[Ee][Xx][Ee]}
	meson_forward=${meson_forward%.[Ee][Xx][Ee]}
	source_windows=$(ninja_escape "$source_windows")
	source_forward=$(ninja_escape "$source_forward")
	meson_windows=$(ninja_escape "$meson_windows")
	meson_forward=$(ninja_escape "$meson_forward")

	if ! grep -Fqim1 -- "\"$source_windows\"" "$build_dir/build.ninja" \
	   && ! grep -Fqim1 -- "\"$source_forward\"" "$build_dir/build.ninja"; then
		return 1
	fi
	grep -Fqim1 -- "\"$meson_windows" "$build_dir/build.ninja" \
		|| grep -Fqim1 -- "\"$meson_forward" "$build_dir/build.ninja"
}

options_match() {
	[[ -f "$coredata" && -f "$build_dir/build.ninja" && -f "$command_line" ]] || return 1
	local line key value
	declare -A options=()
	while IFS= read -r line; do
		line=${line%$'\r'}
		[[ "$line" == *" = "* ]] || continue
		key=${line%% = *}
		value=${line#* = }
		options["$key"]=$value
	done < "$command_line"

	[[ "${options[build_profile]-}" == "$build_profile"
	   && "${options[install_data]-}" == true
	   && "${options[install_dlls]-}" == true
	   && "${options[download_gamepacks]-}" == no
	   && "${options[gamepack_source]-}" == VibePack
	   && "${options[qt_major]-}" == "$qt_major"
	   && "${options[cpp_std]-}" == c++20
	   && "${options[prefix]-}" == "$expected_prefix" ]]
}

setup_options=(
	--prefix "$install_prefix"
	"-Dbuild_profile=$build_profile"
	-Dinstall_data=true
	-Dinstall_dlls=true
	-Ddownload_gamepacks=no
	-Dgamepack_source=VibePack
	"-Dqt_major=$qt_major"
	-Dcpp_std=c++20
)
configured_this_run=false

if [[ -f "$coredata" && ! -f "$context_file" ]] && ! legacy_context_matches; then
	echo "error: cannot prove that $build_dir uses this source tree and MSYS2 toolchain" >&2
	echo "Run the matching clean rebuild task or choose a separate build directory." >&2
	exit 1
fi

if [[ -f "$context_file" ]] && ! context_matches; then
	echo "error: $build_dir belongs to a different source tree or MSYS2 toolchain" >&2
	echo "Run the matching clean rebuild task or choose a separate build directory." >&2
	exit 1
fi

if ! options_match || ! context_matches; then
	if [[ -f "$coredata" ]]; then
		meson setup "$build_dir" --reconfigure "${setup_options[@]}"
	else
		meson setup "$build_dir" --backend=ninja "${setup_options[@]}"
	fi
	context_tmp="$context_file.tmp"
	printf '%s\n' "$expected_context" > "$context_tmp"
	mv -f -- "$context_tmp" "$context_file"
	configured_this_run=true
fi

if [[ ! -f "$build_dir/build.ninja" ]]; then
	echo "error: $build_dir does not use the required Ninja backend" >&2
	exit 1
fi

if [[ "$jobs" == 0 ]]; then
	ninja -C "$build_dir"
else
	ninja -C "$build_dir" -j "$jobs"
fi

if [[ "$install_runtime" == true ]]; then
	build_dir_real=$(CDPATH= cd -- "$build_dir" && pwd -P)
	install_context_file="$install_prefix/.viberadiant-meson-install-context"
	expected_install_context="source=$source_dir"$'\n'"build_dir=$build_dir_real"$'\n'"profile=$build_profile"$'\n'"qt_major=$qt_major"$'\n'"msys2_root=$msys2_root"$'\n'"subsystem=$subsystem"
	install_options=(--no-rebuild --tags runtime)
	if [[ "$configured_this_run" == false
	   && -f "$install_context_file"
	   && "$(< "$install_context_file")" == "$expected_install_context" ]]; then
		install_options+=(--only-changed)
	fi

	# A failed/interrupted install must force a complete repair on the next run.
	rm -f -- "$install_context_file"
	meson install -C "$build_dir" "${install_options[@]}"
	mkdir -p -- "$install_prefix"
	install_context_tmp=$(mktemp "$install_context_file.tmp.XXXXXX")
	trap 'rm -f -- "$install_context_tmp"' EXIT
	printf '%s\n' "$expected_install_context" > "$install_context_tmp"
	mv -f -- "$install_context_tmp" "$install_context_file"
	install_context_tmp=
	trap - EXIT
fi
