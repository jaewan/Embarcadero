#!/bin/bash
# Verify cache-line alignment of critical CXL structures
# Uses pahole to analyze compiled structures

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
REPORT_FILE="$PROJECT_ROOT/data/performance_baseline/cache_alignment_$(date +%Y%m%d_%H%M%S).txt"

echo "=========================================="
echo "Cache-Line Alignment Verification"
echo "=========================================="
echo ""

# Check if pahole is available
if ! command -v pahole &> /dev/null; then
    echo "ERROR: pahole is not installed"
    echo "Install with: sudo apt-get install pahole"
    echo ""
    echo "Alternatively, we can verify alignment using static_assert in code"
    exit 1
fi

# Check if build exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "ERROR: Build directory not found. Please build first:"
    echo "  cd build && make -j\$(nproc)"
    exit 1
fi

# Find object files
TOPIC_OBJ=$(find "$BUILD_DIR" -name "topic.cc.o" -o -name "topic.o" | head -1)
CXL_DS_OBJ=$(find "$BUILD_DIR" -name "cxl_datastructure.o" -o -name "cxl_datastructure.cc.o" | head -1)

if [ -z "$TOPIC_OBJ" ] && [ -z "$CXL_DS_OBJ" ]; then
    echo "ERROR: Object files not found. Please build first:"
    echo "  cd build && make -j\$(nproc)"
    exit 1
fi

mkdir -p "$(dirname "$REPORT_FILE")"

echo "Analyzing cache-line alignment..."
echo "Report: $REPORT_FILE"
echo ""

{
    echo "Cache-Line Alignment Verification Report"
    echo "========================================"
    echo "Date: $(date)"
    echo ""
    
    # Analyze offset_entry
    if [ -n "$CXL_DS_OBJ" ]; then
        echo "=== offset_entry Structure ==="
        echo ""
        pahole -C offset_entry "$CXL_DS_OBJ" 2>/dev/null || echo "offset_entry not found in object file"
        echo ""
        
        # Check size and alignment
        SIZE=$(pahole -C offset_entry -S "$CXL_DS_OBJ" 2>/dev/null | grep -oE "size.*[0-9]+" | head -1 || echo "unknown")
        echo "Size: $SIZE"
        echo "Expected: 512 bytes (alignas(256))"
        echo ""
    fi
    
    # Analyze TInode
    if [ -n "$CXL_DS_OBJ" ]; then
        echo "=== TInode Structure ==="
        echo ""
        pahole -C TInode "$CXL_DS_OBJ" 2>/dev/null || echo "TInode not found in object file"
        echo ""
        
        SIZE=$(pahole -C TInode -S "$CXL_DS_OBJ" 2>/dev/null | grep -oE "size.*[0-9]+" | head -1 || echo "unknown")
        echo "Size: $SIZE"
        echo "Expected: Cache-line aligned (64 bytes)"
        echo ""
    fi
    
    # Analyze BatchHeader
    if [ -n "$CXL_DS_OBJ" ]; then
        echo "=== BatchHeader Structure ==="
        echo ""
        pahole -C BatchHeader "$CXL_DS_OBJ" 2>/dev/null || echo "BatchHeader not found in object file"
        echo ""
        
        SIZE=$(pahole -C BatchHeader -S "$CXL_DS_OBJ" 2>/dev/null | grep -oE "size.*[0-9]+" | head -1 || echo "unknown")
        echo "Size: $SIZE"
        echo "Expected: Cache-line aligned (64 bytes)"
        echo ""
    fi
    
    # Analyze MessageHeader
    if [ -n "$CXL_DS_OBJ" ]; then
        echo "=== MessageHeader Structure ==="
        echo ""
        pahole -C MessageHeader "$CXL_DS_OBJ" 2>/dev/null || echo "MessageHeader not found in object file"
        echo ""
        
        SIZE=$(pahole -C MessageHeader -S "$CXL_DS_OBJ" 2>/dev/null | grep -oE "size.*[0-9]+" | head -1 || echo "unknown")
        echo "Size: $SIZE"
        echo "Expected: Cache-line aligned (64 bytes)"
        echo ""
    fi
    
    echo "=== Verification Summary ==="
    echo ""
    echo "Critical Structures:"
    echo "  offset_entry: Must be 512 bytes (256-byte aligned sub-structs)"
    echo "  TInode: Must be cache-line aligned (64 bytes)"
    echo "  BatchHeader: Must be cache-line aligned (64 bytes)"
    echo "  MessageHeader: Must be cache-line aligned (64 bytes)"
    echo ""
    echo "False Sharing Prevention:"
    echo "  - Broker and Sequencer regions in offset_entry must be separate (256B each)"
    echo "  - Each region should be on separate cache lines"
    echo ""
    
} | tee "$REPORT_FILE"

echo ""
echo "=========================================="
echo "Verification Complete"
echo "=========================================="
echo "Report: $REPORT_FILE"
echo ""
echo "Next Steps:"
echo "  1. Review alignment - ensure all structures are properly aligned"
echo "  2. Check for false sharing - verify broker/sequencer regions are separate"
echo "  3. If misaligned, add padding or adjust structure definitions"
