#!/bin/bash
set -euo pipefail

# Bundle all dylibs for a self-contained dsco distribution
# Rewrites LC_LOAD_DYLIB paths to use @executable_path/lib/

BINARY="${1:-./dsco}"
BUNDLE_DIR="$(dirname "$BINARY")/lib"
BREW="/opt/homebrew"

echo "=== dsco dylib bundler ==="
echo "Binary: $BINARY"
echo "Bundle: $BUNDLE_DIR"

mkdir -p "$BUNDLE_DIR"

# Map system stubs → Homebrew real dylibs
declare -A DYLIB_MAP=(
    ["/usr/lib/libcurl.4.dylib"]="$BREW/opt/curl/lib/libcurl.4.dylib"
    ["/usr/lib/libsqlite3.dylib"]="$BREW/opt/sqlite/lib/libsqlite3.0.dylib"
    ["/usr/lib/libedit.3.dylib"]="$BREW/opt/libedit/lib/libedit.0.dylib"
    # libSystem.B.dylib — cannot bundle (kernel interface). Skip.
)

# Collect ALL transitive Homebrew deps
collect_deps() {
    local lib="$1"
    otool -L "$lib" 2>/dev/null | awk 'NR>1 {print $1}' | grep -v "^/usr/lib/" | grep -v "$lib"
}

SEEN=()
QUEUE=()

# Seed with direct deps
for sys_path in "${!DYLIB_MAP[@]}"; do
    real="${DYLIB_MAP[$sys_path]}"
    if [[ -f "$real" ]]; then
        QUEUE+=("$real")
    else
        echo "  WARN: $real not found"
    fi
done

# BFS transitive closure
while [[ ${#QUEUE[@]} -gt 0 ]]; do
    current="${QUEUE[0]}"
    QUEUE=("${QUEUE[@]:1}")
    
    # Skip if seen
    base=$(basename "$current")
    skip=false
    for s in "${SEEN[@]:-}"; do
        [[ "$s" == "$base" ]] && skip=true && break
    done
    $skip && continue
    
    SEEN+=("$base")
    
    # Resolve symlinks to real file
    real_path=$(readlink -f "$current" 2>/dev/null || echo "$current")
    if [[ ! -f "$real_path" ]]; then
        echo "  SKIP: $current (not found)"
        continue
    fi
    
    echo "  COPY: $real_path → $BUNDLE_DIR/$base"
    cp "$real_path" "$BUNDLE_DIR/$base"
    chmod 755 "$BUNDLE_DIR/$base"
    
    # Collect transitive deps
    for dep in $(collect_deps "$real_path"); do
        if [[ "$dep" != /usr/lib/* ]] && [[ -f "$dep" ]]; then
            QUEUE+=("$dep")
        fi
    done
done

echo ""
echo "=== Bundled ${#SEEN[@]} dylibs ==="
ls -lh "$BUNDLE_DIR/"

echo ""
echo "=== Rewriting load paths ==="

# Rewrite the main binary
for sys_path in "${!DYLIB_MAP[@]}"; do
    real="${DYLIB_MAP[$sys_path]}"
    base=$(basename "$real")
    # Check if the binary actually links this
    if otool -L "$BINARY" | grep -q "$sys_path"; then
        echo "  $BINARY: $sys_path → @executable_path/lib/$base"
        install_name_tool -change "$sys_path" "@executable_path/lib/$base" "$BINARY" 2>/dev/null || true
    fi
done

# Rewrite each bundled dylib's own ID and deps
for lib in "$BUNDLE_DIR"/*.dylib; do
    base=$(basename "$lib")
    
    # Set ID
    install_name_tool -id "@executable_path/lib/$base" "$lib" 2>/dev/null || true
    
    # Rewrite deps within the bundle
    for dep_path in $(otool -L "$lib" | awk 'NR>1 {print $1}' | grep -v "^/usr/lib/"); do
        dep_base=$(basename "$dep_path")
        if [[ -f "$BUNDLE_DIR/$dep_base" ]] && [[ "$dep_path" != "@executable_path/"* ]]; then
            echo "  $base: $dep_path → @executable_path/lib/$dep_base"
            install_name_tool -change "$dep_path" "@executable_path/lib/$dep_base" "$lib" 2>/dev/null || true
        fi
    done
done

echo ""
echo "=== Verification ==="
echo "Binary deps after rewrite:"
otool -L "$BINARY" | head -10

echo ""
echo "Bundle contents:"
du -sh "$BUNDLE_DIR"
ls -1 "$BUNDLE_DIR/"

echo ""
echo "=== Quick smoke test ==="
"$BINARY" --help 2>&1 | head -3 || echo "WARN: binary may need codesign"

# Re-sign (ad-hoc) since install_name_tool invalidates signature
echo ""
echo "=== Re-signing (ad-hoc) ==="
codesign --force --sign - "$BINARY" 2>&1 || true
for lib in "$BUNDLE_DIR"/*.dylib; do
    codesign --force --sign - "$lib" 2>&1 || true
done

echo ""
echo "=== DONE ==="
echo "Self-contained bundle ready. Move dsco + lib/ together."
