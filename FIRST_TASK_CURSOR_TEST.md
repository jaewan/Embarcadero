# First Task: Testing Cursor with Project Setup

**Date:** 2026-01-24
**Purpose:** Validate Cursor works with our rules and governance system

---

## The First Task

### Task 1.1: Implement CXL Cache Primitives

**Priority:** CRITICAL (Foundation for all refactoring)
**File to create:** `src/common/performance_utils.h`
**Risk level:** Low (new file, doesn't modify existing code)

---

## Expert Validation: Is This the Right First Task?

### ✅ YES - Here's Why

**1. Correctness First**
- CXL memory is **non-cache-coherent**
- Without explicit flushes → stale data reads → silent corruption
- MUST have these primitives before refactoring data structures
- **Expert consensus:** Fix correctness issues before performance optimization

**2. Foundation Before Structure**
- Can't refactor BrokerMetadata without flush primitives
- Can't add cache-line alignment enforcement without flush capability
- This unblocks Tasks 1.2, 2.1, 2.2, etc.
- **Expert consensus:** Build utilities before using them

**3. Low Risk, High Value**
- Creating new header file = minimal risk
- Doesn't touch existing hot paths yet
- Can write unit tests before integration
- Easy to revert if needed
- **Expert consensus:** Start with low-risk, high-value tasks

**4. Testable in Isolation**
- Can verify assembly output (clflushopt, sfence, lfence)
- Can benchmark overhead (should be <1ns per flush)
- Can test on both x86-64 and ARM
- **Expert consensus:** Test primitives thoroughly before integration

---

## What Cursor Should Do (If Working Correctly)

### Step 1: Load Context
When you give the command, Cursor should:
1. ✅ Auto-load `.cursor/rules/00-context-loader.mdc`
2. ✅ See instruction to load `docs/memory-bank/activeContext.md`
3. ✅ Load activeContext.md and see Task 1.1 details
4. ✅ Load `docs/memory-bank/spec_deviation.md` (check for deviations)
5. ✅ Load `docs/memory-bank/paper_spec.md` (reference design)

### Step 2: Follow Code Style Rules
When implementing, Cursor should:
1. ✅ Reference `.cursor/rules/10-code-style.mdc`
2. ✅ Add header documentation with `@threading`, `@ownership`, `@paper_ref`
3. ✅ Use `alignas(64)` if defining CXL structs
4. ✅ Add `static_assert` for size constraints
5. ✅ Mark with `[[PAPER_SPEC: Implemented]]` or `[[DEVIATION_XXX]]`

### Step 3: Check Deviations
Cursor should check `spec_deviation.md`:
1. ✅ Look for DEV-002: Cache Flush Optimization
2. ✅ See it's "📋 Planned" (batched flushes)
3. ✅ Decide: Implement paper design first, add batching later

### Step 4: Implement Correctly
Based on activeContext.md Task 1.1:
1. ✅ Create `src/common/performance_utils.h`
2. ✅ Add namespace `Embarcadero::CXL`
3. ✅ Add functions: `flush_cacheline()`, `store_fence()`, `load_fence()`, `cpu_pause()`
4. ✅ Use x86-64 intrinsics (`_mm_clflushopt`, `_mm_sfence`, etc.)
5. ✅ Add ARM fallback (`__builtin___clear_cache`)
6. ✅ Add architecture detection (`#ifdef __x86_64__`)

### Step 5: Build Verification
After implementation, Cursor should (per 90-rlm-verifier.mdc):
1. ✅ Run build: `cd build && make -j$(nproc)`
2. ✅ Verify compilation succeeds
3. ✅ Suggest creating test file

---

## The Exact Command to Give Cursor

### Option 1: Minimal Command (Tests Rule Loading)

Open Cursor and say:

```
Implement Task 1.1 from activeContext.md: Create src/common/performance_utils.h with CXL cache primitives.

Follow the implementation checklist in activeContext.md lines 65-70.
```

**What to watch for:**
- Does Cursor load activeContext.md? (should reference it)
- Does Cursor follow code style rules? (header docs, static_assert)
- Does Cursor check spec_deviation.md? (should mention DEV-002)

---

### Option 2: Explicit Command (Forces Rule Reading)

```
First, read the following files in order:
1. .cursor/rules/00-context-loader.mdc
2. docs/memory-bank/activeContext.md (Task 1.1)
3. docs/memory-bank/spec_deviation.md (check for cache flush deviations)
4. .cursor/rules/10-code-style.mdc (Rule #8 - Paper Spec Compliance)

Then implement Task 1.1: Create src/common/performance_utils.h with CXL cache primitives.

Requirements:
- Follow activeContext.md implementation checklist (lines 65-70)
- Follow code style rules (header docs, static_assert)
- Check if DEV-002 affects implementation
- Mark code with appropriate [[PAPER_SPEC]] or [[DEVIATION]] markers
```

**What to watch for:**
- Cursor should explicitly confirm reading each file
- Should reference specific rules
- Should ask about DEV-002 (batched flushes)

---

### Option 3: Test Questions (Verify Context Loaded)

Before implementing, ask Cursor:

```
Before we start implementing, answer these questions:
1. What is the specification hierarchy for this project?
2. What are the cache-line alignment requirements?
3. Is there a deviation for cache flush implementation?
4. What files should I read before implementing Task 1.1?
```

**Expected answers:**
1. "spec_deviation.md → paper_spec.md → engineering judgment"
2. "alignas(64) for all CXL structs, Rule #2 from 10-code-style.mdc"
3. "Yes, DEV-002 (batched flushes) is planned but not implemented yet"
4. "activeContext.md, spec_deviation.md, paper_spec.md"

**If Cursor answers correctly:** Rules are loaded ✅
**If Cursor doesn't know:** Rules not loaded ❌

---

## What the Output Should Look Like

### Expected: `src/common/performance_utils.h`

```cpp
// FILE: src/common/performance_utils.h
#pragma once

/**
 * @file performance_utils.h
 * @brief CXL cache coherence primitives for non-coherent memory
 *
 * @threading All functions are thread-safe (CPU instructions)
 * @ownership No memory ownership (operates on caller's addresses)
 * @paper_ref Paper §4.2 - Flush & Poll principle
 */

#include <cstddef>
#include <cstdint>

#ifdef __x86_64__
#include <immintrin.h>  // For _mm_clflushopt, _mm_sfence, etc.
#endif

namespace Embarcadero {
namespace CXL {

/**
 * @brief Flush a cache line containing the given address
 *
 * @threading Thread-safe (CPU instruction)
 * @ownership Does not take ownership of addr
 * @alignment Works on any address (rounds down to cache line)
 * @paper_ref Paper §4.2 - Uses clflushopt for non-coherent CXL
 *
 * @param addr Pointer to address within cache line to flush
 *
 * Implementation:
 * - x86-64: _mm_clflushopt (optimized flush, non-blocking)
 * - ARM64: __builtin___clear_cache (DC CVAC instruction)
 */
inline void flush_cacheline(const void* addr) {
#ifdef __x86_64__
    _mm_clflushopt(addr);
#elif defined(__aarch64__)
    __builtin___clear_cache(
        (char*)((uintptr_t)addr & ~63UL),
        (char*)(((uintptr_t)addr & ~63UL) + 64)
    );
#else
    // Generic fallback - no-op (assume coherent memory)
    (void)addr;
#endif
}

/**
 * @brief Store fence (ensures all prior stores are visible)
 *
 * @threading Thread-safe (CPU instruction)
 * @paper_ref Paper §4.2 - sfence after clflushopt
 *
 * Guarantees:
 * - All stores before this fence complete before stores after
 * - All flushes before this fence complete before stores after
 */
inline void store_fence() {
#ifdef __x86_64__
    _mm_sfence();
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb st" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

/**
 * @brief Load fence (ensures all prior loads are visible)
 *
 * @threading Thread-safe (CPU instruction)
 * @paper_ref Paper §4.2 - lfence for ordering
 */
inline void load_fence() {
#ifdef __x86_64__
    _mm_lfence();
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb ld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief CPU pause hint (for spin loops)
 *
 * @threading Thread-safe (CPU instruction)
 * @paper_ref Paper §3 - Polling loops in receiver/delegation threads
 *
 * Reduces power consumption and improves spin-wait performance
 */
inline void cpu_pause() {
#ifdef __x86_64__
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    // No-op on unknown architectures
#endif
}

// [[PAPER_SPEC: Implemented]] - Matches Paper §4.2 cache coherence protocol
// Note: DEV-002 (batched flushes) is planned but not implemented yet

} // namespace CXL
} // namespace Embarcadero
```

### Expected Code Style Elements

**✅ Should include:**
1. Header documentation with `@threading`, `@ownership`, `@paper_ref`
2. Function documentation for each primitive
3. Architecture detection (`#ifdef __x86_64__`)
4. ARM fallbacks
5. Inline functions (no runtime overhead)
6. `[[PAPER_SPEC: Implemented]]` marker
7. Reference to DEV-002 (batched flushes planned)
8. Namespace `Embarcadero::CXL`

**❌ Should NOT include:**
1. No `alignas(64)` on these functions (they're not structs)
2. No `static_assert` (nothing to assert here)
3. No manual destructors
4. No magic numbers

---

## How to Verify Cursor Worked Correctly

### Check 1: Rules Referenced

**Look in Cursor's response for:**
```
Based on activeContext.md Task 1.1...
Following .cursor/rules/10-code-style.mdc...
Checking spec_deviation.md for deviations...
```

**✅ PASS:** Cursor references the rules
**❌ FAIL:** Cursor doesn't mention rules → Not loading context

---

### Check 2: Code Style Compliance

**Check the generated code has:**
- [ ] Header documentation with `@threading`, `@ownership`, `@paper_ref`
- [ ] Function documentation
- [ ] `[[PAPER_SPEC: Implemented]]` marker
- [ ] Namespace `Embarcadero::CXL`
- [ ] Architecture detection
- [ ] ARM fallbacks

**✅ PASS:** 5+ items checked
**❌ FAIL:** <3 items checked → Not following code style

---

### Check 3: Specification Hierarchy

**Check if Cursor:**
- [ ] Mentions spec_deviation.md
- [ ] Mentions DEV-002 (batched flushes)
- [ ] Decides to implement paper design first (DEV-002 is planned, not implemented)

**✅ PASS:** Cursor checks deviations before implementing
**❌ FAIL:** Cursor doesn't mention spec_deviation.md

---

### Check 4: Build Verification

**After Cursor generates code:**

```bash
cd /home/domin/Embarcadero/build
make -j$(nproc)
```

**Expected:**
```
[ 98%] Building CXX object src/common/...
[100%] Built target embarlet
```

**✅ PASS:** Compiles without errors
**❌ FAIL:** Compilation errors → Check includes, syntax

---

### Check 5: Pre-Commit Hook

**Test the pre-commit hook:**

```bash
git add src/common/performance_utils.h
.git/hooks/pre-commit
```

**Expected:**
```
Running Embarcadero pre-commit checks...
Checking src/common/performance_utils.h

[1/4] Checking cache-line alignment...
[2/4] Checking cache flushes...
[3/4] Checking for manual destructors...
[4/4] Checking for magic numbers...

✓ Pre-commit checks completed
```

**✅ PASS:** Hook runs without warnings
**❌ FAIL:** Hook reports issues → Fix issues

---

## Troubleshooting

### Issue 1: Cursor Doesn't Reference Rules

**Symptoms:**
- Cursor doesn't mention activeContext.md
- Cursor doesn't check spec_deviation.md
- Code doesn't follow style rules

**Solution:**
```
Cursor, please read the following files first:
1. .cursor/rules/00-context-loader.mdc
2. docs/memory-bank/activeContext.md

Then tell me what Task 1.1 requires.
```

**If still doesn't work:**
- Restart Cursor
- Check `.cursor/rules/00-context-loader.mdc` exists
- Check `alwaysApply: true` in frontmatter

---

### Issue 2: Cursor Generates Wrong Code

**Symptoms:**
- Missing header documentation
- Missing `[[PAPER_SPEC]]` markers
- Wrong namespace

**Solution:**
```
Regenerate the file following .cursor/rules/10-code-style.mdc Rule #8.

Requirements:
- Add @threading, @ownership, @paper_ref to header
- Use namespace Embarcadero::CXL
- Mark with [[PAPER_SPEC: Implemented]]
```

---

### Issue 3: Build Fails

**Symptoms:**
- Compilation errors
- Missing includes

**Solution:**
```bash
# Check what failed
cd build
make VERBOSE=1

# Common issues:
# - Missing #include <immintrin.h>
# - Wrong path in #include
# - Syntax errors
```

**Fix and rebuild:**
```
Cursor, the build failed with this error: [paste error]
Please fix src/common/performance_utils.h
```

---

## Success Criteria

### ✅ Cursor Working Perfectly If:

1. **Context Loaded:**
   - References activeContext.md Task 1.1
   - Checks spec_deviation.md for DEV-002
   - Mentions paper_spec.md §4.2

2. **Code Style Followed:**
   - Header docs with `@threading`, `@ownership`, `@paper_ref`
   - `[[PAPER_SPEC: Implemented]]` marker
   - Namespace `Embarcadero::CXL`

3. **Specification Hierarchy:**
   - Checks deviations first
   - Falls back to paper design
   - Documents choice

4. **Build Succeeds:**
   - Compiles without errors
   - Pre-commit hook passes

5. **Expert Quality:**
   - x86-64 intrinsics correct
   - ARM fallbacks present
   - Inline functions (no overhead)

---

## Next Steps After Success

### 1. Test the Primitives

**Create unit test:**
```bash
# Cursor should suggest this (Rule 90-rlm-verifier.mdc)
mkdir -p test/common
# Create test file
```

**Test commands:**
```cpp
// test/common/performance_utils_test.cc
#include "common/performance_utils.h"
#include <gtest/gtest.h>

TEST(CXLPrimitives, FlushCacheline) {
    alignas(64) char buffer[64];
    Embarcadero::CXL::flush_cacheline(buffer);
    // Should not crash
}

TEST(CXLPrimitives, StoreFence) {
    Embarcadero::CXL::store_fence();
    // Should not crash
}
```

---

### 2. Proceed to Task 1.2

**Once Task 1.1 is complete and tested:**

```
Now implement Task 1.2 from activeContext.md:
Integrate cache flushes into hot path.

Start with CombinerThread in src/embarlet/topic.cc:30859
```

---

### 3. Update activeContext.md

**Mark Task 1.1 as complete:**
```
Update docs/memory-bank/activeContext.md:
- Change line 49 from [ ] to [x]
- Update "Last Updated" to today's date
```

---

## Summary

### The Command (Recommended)

**Give Cursor this command:**

```
Implement Task 1.1 from activeContext.md: Create src/common/performance_utils.h with CXL cache primitives.

Follow:
1. activeContext.md lines 65-70 (implementation checklist)
2. .cursor/rules/10-code-style.mdc (code style)
3. spec_deviation.md (check for DEV-002)

Include:
- Header docs with @threading, @ownership, @paper_ref
- x86-64 intrinsics and ARM fallbacks
- [[PAPER_SPEC: Implemented]] marker
- Reference to DEV-002 (batched flushes planned)
```

### What Should Happen

1. Cursor loads activeContext.md
2. Cursor checks spec_deviation.md
3. Cursor follows code style rules
4. Cursor generates high-quality code
5. Build succeeds
6. Pre-commit hook passes

### If It Works

**You've validated:**
- ✅ `.cursor/rules/` is working
- ✅ `docs/memory-bank/` is loaded
- ✅ Specification hierarchy is followed
- ✅ Code style is enforced
- ✅ Cursor is ready for refactoring

### If It Doesn't Work

**Debug:**
1. Check `.cursor/rules/*.mdc` files exist
2. Restart Cursor
3. Use explicit command (Option 2)
4. Verify with test questions (Option 3)

---

**Status:** Ready for testing
**Estimated time:** 10-15 minutes
**Risk:** Low (new file, doesn't modify existing code)
**Reward:** Validation of entire setup + foundation for refactoring
