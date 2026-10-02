# This script bundles the dynamic libraries needed by the binary into the
# app bundle. It is optional – the app can be run directly from the build
# directory without bundling if the required libraries are already available in
# the system (e.g., when using Homebrew on macOS). The script is kept for users
# who need a fully self‑contained .app.
set -e

APP_EXEC="$1"
FRAMEWORKS_DIR="$2"

if [ -z "$APP_EXEC" ] || [ -z "$FRAMEWORKS_DIR" ]; then
    echo "Usage: bundle_deps.sh <app_executable> <frameworks_dir>"
    exit 1
fi

if [ ! -f "$APP_EXEC" ]; then
    echo "Error: executable not found: $APP_EXEC"
    exit 1
fi

mkdir -p "$FRAMEWORKS_DIR"

VISITED=$(mktemp)
trap "rm -f '$VISITED'" EXIT

collect_deps() {
    local lib="$1"

    if grep -qF "$lib" "$VISITED" 2>/dev/null; then
        return
    fi
    echo "$lib" >> "$VISITED"

    otool -L "$lib" 2>/dev/null | grep '\.dylib' | awk '{print $1}' | while read -r dep; do
        if [ -f "$dep" ]; then
            echo "$dep"
            collect_deps "$dep"
        fi
    done
}

echo "Collecting dependencies..."
DEPS=$(collect_deps "$APP_EXEC" | sort -u)

COPIED=0
for lib in $DEPS; do
    if [ -f "$lib" ]; then
        basename_lib=$(basename "$lib")
        dest="$FRAMEWORKS_DIR/$basename_lib"
        cp -f "$lib" "$dest"
        chmod +rw "$dest"
        COPIED=$((COPIED + 1))
    fi
done

echo "Copied $COPIED libraries to $FRAMEWORKS_DIR"

echo "Fixing install names..."

for lib in $DEPS; do
    if [ -f "$lib" ]; then
        basename_lib=$(basename "$lib")
        dest="$FRAMEWORKS_DIR/$basename_lib"

        install_name_tool -id "@rpath/$basename_lib" "$dest" 2>/dev/null || true

        otool -L "$dest" 2>/dev/null | grep '\.dylib' | awk '{print $1}' | while read -r dep; do
            dep_basename=$(basename "$dep")
            if [ -f "$FRAMEWORKS_DIR/$dep_basename" ]; then
                install_name_tool -change "$dep" "@loader_path/$dep_basename" "$dest" 2>/dev/null || true
            fi
        done
    fi
done

install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP_EXEC" 2>/dev/null || true

otool -L "$APP_EXEC" 2>/dev/null | grep '\.dylib' | awk '{print $1}' | while read -r dep; do
    dep_basename=$(basename "$dep")
    if [ -f "$FRAMEWORKS_DIR/$dep_basename" ]; then
        install_name_tool -change "$dep" "@executable_path/../Frameworks/$dep_basename" "$APP_EXEC" 2>/dev/null || true
    fi
done

echo "Bundle preparation complete."
