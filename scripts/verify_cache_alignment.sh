#!/bin/bash
# Embarcadero Cache Alignment Verifier
# Scans all CXL structs and verifies 64-byte alignment
#
# Usage: ./scripts/verify_cache_alignment.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "======================================"
echo "Embarcadero Cache Alignment Verifier"
echo "======================================"
echo ""

# Check if pahole is available
if ! command -v pahole &> /dev/null; then
    echo "WARNING: pahole not found. Install with: sudo apt install dwarves"
    echo "Falling back to source code analysis only..."
    echo ""
    USE_PAHOLE=false
else
    USE_PAHOLE=true
fi

# Find all header files
HEADER_FILES=$(find "$PROJECT_ROOT/src" -name "*.h" 2>/dev/null || true)

if [ -z "$HEADER_FILES" ]; then
    echo "No header files found in src/"
    exit 0
fi

echo "Scanning header files for CXL structs..."
echo ""

TOTAL_STRUCTS=0
ALIGNED_STRUCTS=0
UNALIGNED_STRUCTS=0
WARNINGS=()

# Scan each header file
for file in $HEADER_FILES; do
    rel_path="${file#$PROJECT_ROOT/}"

    # Find struct definitions that look like CXL-related
    while IFS= read -r line_num; do
        if [ -z "$line_num" ]; then
            continue
        fi

        TOTAL_STRUCTS=$((TOTAL_STRUCTS + 1))

        # Extract struct name
        struct_line=$(sed -n "${line_num}p" "$file")
        struct_name=$(echo "$struct_line" | grep -oP 'struct\s+\K\w+' || echo "unnamed")

        # Check if alignas(64) appears before the struct
        context_start=$((line_num - 5))
        if [ $context_start -lt 1 ]; then
            context_start=1
        fi

        context=$(sed -n "${context_start},${line_num}p" "$file")

        if echo "$context" | grep -q 'alignas(64)'; then
            ALIGNED_STRUCTS=$((ALIGNED_STRUCTS + 1))
            echo "✓ $rel_path:$line_num - struct $struct_name (aligned)"
        else
            UNALIGNED_STRUCTS=$((UNALIGNED_STRUCTS + 1))
            echo "⚠️  $rel_path:$line_num - struct $struct_name (NOT aligned)"
            WARNINGS+=("$rel_path:$line_num - struct $struct_name")
        fi

    done < <(grep -n 'struct.*\(Broker\|Meta\|TInode\|Bmeta\|Blog\|Message\|CXL\|Header\).*{' "$file" | cut -d: -f1 || true)
done

echo ""
echo "======================================"
echo "Summary"
echo "======================================"
echo "Total CXL structs found: $TOTAL_STRUCTS"
echo "  ✓ Aligned (alignas(64)): $ALIGNED_STRUCTS"
echo "  ⚠️  Not aligned: $UNALIGNED_STRUCTS"
echo ""

if [ $UNALIGNED_STRUCTS -gt 0 ]; then
    echo "Unaligned structs:"
    for warning in "${WARNINGS[@]}"; do
        echo "  - $warning"
    done
    echo ""
    echo "Recommendation: Add alignas(64) to shared memory structs"
    echo "See .cursor/rules/10-code-style.mdc Rule #2"
    echo ""
fi

# If pahole is available, verify actual binary layout
if [ "$USE_PAHOLE" = true ]; then
    echo "======================================"
    echo "Binary Analysis (pahole)"
    echo "======================================"
    echo ""

    # Find compiled object files or archives
    BUILD_ARTIFACTS=$(find "$PROJECT_ROOT/build" -name "*.a" -o -name "*.o" 2>/dev/null | head -n 5 || true)

    if [ -z "$BUILD_ARTIFACTS" ]; then
        echo "No build artifacts found in build/"
        echo "Run 'cd build && make' first to verify with pahole"
    else
        echo "Checking struct sizes in compiled binaries..."
        echo ""

        for artifact in $BUILD_ARTIFACTS; do
            # Extract struct names and check their sizes
            pahole "$artifact" 2>/dev/null | grep -A1 "struct.*Meta\|struct.*Broker\|struct.*TInode" | head -n 20 || true
        done
    fi
fi

echo ""
echo "======================================"
echo "Verification complete"
echo "======================================"

if [ $UNALIGNED_STRUCTS -gt 0 ]; then
    exit 1
else
    echo "✓ All CXL structs are properly aligned"
    exit 0
fi
