#!/bin/bash
# Static analysis script for detecting lock ordering violations in TUI code
# This script helps identify potential deadlock risks by analyzing mutex acquisition patterns

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TUI_FILE="$PROJECT_ROOT/src/tui.c"

echo "=== TUI Lock Ordering Analysis ==="
echo "Analyzing: $TUI_FILE"
echo

# Define lock patterns to search for
TERM_LOCK_PATTERN="tui_term_lock\(\)"
TERM_UNLOCK_PATTERN="tui_term_unlock\(\)"
OUTQ_LOCK_PATTERN="pthread_mutex_lock.*q->mutex"
OUTQ_UNLOCK_PATTERN="pthread_mutex_unlock.*q->mutex"

echo "1. Checking for lock hierarchy violations..."
echo

# Search for potential violations: holding OUTQ lock while acquiring TERM lock
echo "   Scanning for: q->mutex → g_term_mutex violations (OUTQ→TERM)"
violations=$(grep -n -B 3 "$TERM_LOCK_PATTERN" "$TUI_FILE" | grep -B 3 "pthread_mutex_lock.*q->mutex" || true)

if [ -n "$violations" ]; then
    echo "   ⚠️  POTENTIAL VIOLATIONS FOUND:"
    echo "$violations"
    echo
    echo "   ❌ These sections may violate lock hierarchy (holding OUTQ while acquiring TERM)"
else
    echo "   ✓ No OUTQ→TERM violations detected in current code"
fi

echo

# Check for proper lock validation macro usage
echo "2. Checking for lock validation macro usage..."
echo

if grep -q "LOCK_ACQUIRE\|LOCK_RELEASE" "$TUI_FILE"; then
    echo "   ✓ Lock validation macros present"
    macro_count=$(grep -c "LOCK_ACQUIRE\|LOCK_RELEASE" "$TUI_FILE")
    echo "   Found $macro_count macro usage(s)"
else
    echo "   ⚠️  Lock validation macros not found (add with -DDEBUG_LOCKS for debugging)"
fi

echo

# Check for nested lock acquisitions
echo "3. Checking for nested lock acquisitions..."
echo

nested_locks=$(grep -n -A 2 "pthread_mutex_lock" "$TUI_FILE" | grep -A 2 "pthread_mutex_lock" | grep -v "^--$" || true)

if [ -n "$nested_locks" ]; then
    echo "   ⚠️  Nested lock acquisitions detected:"
    echo "$nested_locks"
    echo
    echo "   Review these sections for potential lock hierarchy violations"
else
    echo "   ✓ No obvious nested lock patterns detected"
fi

echo

# Summary
echo "=== Analysis Summary ==="
echo
echo "For comprehensive deadlock detection, compile with ThreadSanitizer:"
echo "  clang -fsanitize=thread -g -o dsco-san ./src/tui.c ..."
echo
echo "Then run the sanitized binary under heavy load."
echo

# Offer to compile with ThreadSanitizer
read -p "Compile with ThreadSanitizer now? (y/N) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Compiling with ThreadSanitizer..."
    cd "$PROJECT_ROOT"
    make clean
    CFLAGS="-fsanitize=thread -g" make
    echo "✓ ThreadSanitizer build complete: ./dsco"
    echo "Run with: ./dsco (monitor for deadlock warnings)"
fi