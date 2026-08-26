#!/bin/sh

# set -ex

INSTALLDIR=${INSTALLDIR:-"$(pwd)/install"}

for REQUIRED_TOOL in file objdump sed sort comm cp mktemp; do
    if ! command -v "$REQUIRED_TOOL" >/dev/null 2>&1; then
        printf 'Required runtime-staging tool is unavailable: %s\n' "$REQUIRED_TOOL" >&2
        exit 1
    fi
done

if [ ! -d "$INSTALLDIR" ]; then
    printf 'Install directory does not exist: %s\n' "$INSTALLDIR" >&2
    exit 1
fi

# Keep INSTALLDIR valid after changing to the MinGW prefix below. This also
# avoids passing a path containing spaces through unquoted shell expansion.
INSTALLDIR=$(CDPATH= cd "$INSTALLDIR" && pwd -P) || exit 1

if [ ! -f "$INSTALLDIR/radiant.exe" ]; then
    printf 'Installed editor executable does not exist: %s\n' "$INSTALLDIR/radiant.exe" >&2
    exit 1
fi

case $(file "$INSTALLDIR/radiant.exe" 2>/dev/null) in
    *x86-64*) MINGWDIR=/mingw64 ;;
    *80386*) MINGWDIR=/mingw32 ;;
    *)
        printf 'Unable to determine editor architecture: %s\n' "$INSTALLDIR/radiant.exe" >&2
        exit 1
        ;;
esac

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/install-dlls-msys2-mingw.XXXXXX") || {
    printf 'Could not create a temporary directory.\n' >&2
    exit 1
}

cleanup() {
    rm -rf "$WORKDIR"
}

trap cleanup 0
trap 'exit 1' 1 2 3 15

dependencies_single_target_no_depth() {
    objdump -x "$1" 2>/dev/null |
        sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' |
        while IFS= read -r DLL_NAME; do
            DLL_PATH="$MINGWDIR/bin/$DLL_NAME"
            [ -f "$DLL_PATH" ] && printf '%s\n' "$DLL_PATH"
        done
}

dependencies() {
    ALL_DEPENDENCIES="$WORKDIR/all-dependencies"
    CANDIDATES="$WORKDIR/candidates"
    DISCOVERED="$WORKDIR/discovered"
    DISCOVERED_SORTED="$WORKDIR/discovered-sorted"
    NEW_DEPENDENCIES="$WORKDIR/new-dependencies"
    MERGED="$WORKDIR/merged"

    : > "$CANDIDATES"
    while IFS= read -r TARGET; do
        [ -f "$TARGET" ] || continue
        dependencies_single_target_no_depth "$TARGET" >> "$CANDIDATES"
    done < "$1"

    sort -u "$CANDIDATES" > "$ALL_DEPENDENCIES"
    cp "$ALL_DEPENDENCIES" "$NEW_DEPENDENCIES"

    while [ -s "$NEW_DEPENDENCIES" ]; do
        : > "$DISCOVERED"
        while IFS= read -r DEPENDENCY; do
            [ -n "$DEPENDENCY" ] || continue
            dependencies_single_target_no_depth "$DEPENDENCY" >> "$DISCOVERED"
        done < "$NEW_DEPENDENCIES"

        sort -u "$DISCOVERED" > "$DISCOVERED_SORTED"
        comm -13 "$ALL_DEPENDENCIES" "$DISCOVERED_SORTED" > "$NEW_DEPENDENCIES"
        [ -s "$NEW_DEPENDENCIES" ] || break

        {
            cat "$ALL_DEPENDENCIES"
            cat "$NEW_DEPENDENCIES"
        } | sort -u > "$MERGED"
        mv "$MERGED" "$ALL_DEPENDENCIES"
    done

    cat "$ALL_DEPENDENCIES"
}

cd "$MINGWDIR" || exit 1

QT_PLUGINS_DIR=./share/qt6/plugins
if [ ! -d "$QT_PLUGINS_DIR" ]; then
    QT_PLUGINS_DIR=./share/qt5/plugins
fi

PLUGIN_DLLS="$WORKDIR/plugin-dlls"
SEED_BINARIES="$WORKDIR/seed-binaries"
RESOLVED_DEPENDENCIES="$WORKDIR/resolved-dependencies"
: > "$PLUGIN_DLLS"
: > "$SEED_BINARIES"

for PLUGIN in \
    "$QT_PLUGINS_DIR"/imageformats/*.dll \
    "$QT_PLUGINS_DIR"/platforms/*.dll \
    "$QT_PLUGINS_DIR"/styles/*.dll \
    "$QT_PLUGINS_DIR"/iconengines/*.dll \
    "$QT_PLUGINS_DIR"/tls/*.dll \
    "$QT_PLUGINS_DIR"/ssl/*.dll \
    "$QT_PLUGINS_DIR"/multimedia/*.dll
do
    [ -f "$PLUGIN" ] || continue
    printf '%s\n' "$PLUGIN" >> "$PLUGIN_DLLS"
done

for SEED in \
    "$INSTALLDIR"/*.exe \
    "$INSTALLDIR"/modules/*.dll \
    "$INSTALLDIR"/plugins/*.dll \
    "$INSTALLDIR"/ericw/*.exe
do
    [ -f "$SEED" ] || continue
    printf '%s\n' "$SEED" >> "$SEED_BINARIES"
done
cat "$PLUGIN_DLLS" >> "$SEED_BINARIES"

dependencies "$SEED_BINARIES" > "$RESOLVED_DEPENDENCIES"

COPY_STATUS=0
while IFS= read -r DEPENDENCY; do
    [ -n "$DEPENDENCY" ] || continue
    cp -v "$DEPENDENCY" "$INSTALLDIR" || COPY_STATUS=1
done < "$RESOLVED_DEPENDENCIES"

while IFS= read -r PLUGIN; do
    [ -n "$PLUGIN" ] || continue
    cp --parents -v "$PLUGIN" "$INSTALLDIR" || COPY_STATUS=1
done < "$PLUGIN_DLLS"

exit "$COPY_STATUS"
