# AI Code Style Analysis for Embarcadero

**Date:** 2026-01-24
**Question:** Is this the best practice for AI-assisted development on Embarcadero?

---

## Executive Summary

**Answer: YES**, with one important addition.

The combination of:
1. Context loader (`00-context-loader.mdc`)
2. Embarcadero-specific code style (`10-code-style.mdc`)
3. Build verification (`90-rlm-verifier.mdc`)

...provides an **excellent foundation** for AI-assisted development on this project.

**Recommended Addition:** Add a pre-commit hook to enforce critical rules.

---

## Analysis of Current Rules

### ✅ What's Excellent

#### 1. Context Loader (00-context-loader.mdc)

**What it does:**
- Forces AI to load Memory Bank before any work
- Points to codebase_map.xml for navigation
- Ensures AI understands project constraints

**Why it's best practice:**
- **Persistent context:** AI always knows about Paper Spec, Four Laws, migration status
- **Prevents mistakes:** AI won't violate cache-line alignment if it reads paper_spec.md first
- **Navigation efficiency:** codebase_map.xml prevents "blind searching"

**Grade: A+**

This is **critical** for this project because:
- False sharing bugs are subtle and hard to debug
- Paper spec compliance requires understanding the "why" not just "what"
- Migration from TInode → Bmeta needs coordination across files

---

#### 2. Embarcadero Code Style (10-code-style.mdc)

**What it does:**
- Enforces cache-line alignment requirements
- Requires concurrency annotations
- Documents cache flush requirements
- Prevents false sharing
- Tracks Paper Spec compliance

**Why it's best practice for THIS project:**

| Rule | Why Critical for Embarcadero | Alternative (Worse) |
|:-----|:----------------------------|:--------------------|
| Cache-line alignment | CXL non-coherent memory → false sharing = silent corruption | Trust developers (fails) |
| Writer annotations | Single Writer Principle verification | Comments (AI ignores) |
| Cache flush requirement | Other hosts see stale data without flush | Hope (fails) |
| Paper markers | Track migration progress | External spreadsheet (stale) |
| Ownership docs | Prevents use-after-free in complex lifecycle | Assume (fails) |

**Example of why this matters:**

**Without rule:**
```cpp
// AI sees this, generates "optimized" version:
struct BrokerMeta {
    uint64_t broker_field;
    uint64_t sequencer_field;  // AI doesn't know this is wrong!
};
```

**With rule:**
```cpp
// AI knows to check cache-line boundaries:
struct alignas(64) BrokerMeta {
    // [[WRITER: Broker]]
    uint64_t broker_field;

    // [[WRITER: Sequencer]] ⚠️ Rule flags: different writer, must be different cache line!
    uint64_t sequencer_field;  // ERROR: Same cache line as broker_field!
};
```

**Grade: A+**

This is **project-specific** and encodes knowledge that generic AI models don't have.

---

#### 3. RLM Verifier (90-rlm-verifier.mdc)

**What it does:**
- Forces build verification after C++ changes
- Enforces test creation for new logic
- Cleanup requirement (deletes temp files)

**Why it's best practice:**
- **Catches errors immediately:** Compilation errors don't make it to commits
- **Test-driven:** Forces verification of behavior
- **Clean workspace:** Prevents cruft accumulation

**Grade: A**

Good, but could be enhanced (see recommendations below).

---

### ⚠️ What Could Be Better

#### 1. Missing: Pre-commit Hook

**Problem:** Rules are guidance, not enforcement.

**Current state:**
- AI "should" check cache alignment
- But nothing STOPS a commit if it doesn't

**Solution:** Add `.cursor/hooks/pre-commit.sh`:

```bash
#!/bin/bash
# Runs before every commit

echo "Running Embarcadero pre-commit checks..."

# Check 1: All CXL structs have alignas(64)
echo "Checking cache-line alignment..."
git diff --cached --name-only | grep -E '\.(h|cc)$' | while read file; do
    if git diff --cached "$file" | grep -E 'struct.*\{' | grep -v 'alignas(64)' | grep -q 'CXL\|Broker\|TInode'; then
        echo "ERROR: CXL struct without alignas(64) in $file"
        echo "See .cursor/rules/10-code-style.mdc #2"
        exit 1
    fi
done

# Check 2: CXL writes have cache flush
echo "Checking cache flushes..."
if git diff --cached | grep -E 'msg_header->|tinode->' | grep -v 'flush_cacheline'; then
    echo "WARNING: CXL write without flush_cacheline()"
    echo "See .cursor/rules/10-code-style.mdc #4"
    echo "Continue? (y/n)"
    read answer
    [ "$answer" != "y" ] && exit 1
fi

# Check 3: No manual destructors
echo "Checking for manual destructor calls..."
if git diff --cached | grep -E '~[A-Z][a-zA-Z]*\(\)'; then
    echo "ERROR: Manual destructor call detected"
    echo "See E2E_TEST_FIXES_COMPLETE.md"
    exit 1
fi

echo "✓ All pre-commit checks passed"
```

**Benefit:** **Prevents** bugs instead of **detecting** them after commit.

---

#### 2. Enhancement: Add Complexity Budget

**Current:** No guidance on when to break down functions

**Add to 10-code-style.mdc:**

```markdown
## 16. COMPLEXITY BUDGET

### Rule: Hot Path Functions < 50 Lines

**REQUIRED: Break down if exceeding budget:**
- Hot path functions: Max 50 lines
- Cold path functions: Max 200 lines
- Constructors: Max 100 lines

**Why:** AI struggles to reason about 500-line functions.

**Example:**
```cpp
// ❌ WRONG: 300-line hot path function
void ProcessMessage(...) {
    // 300 lines of complex logic
}

// ✅ CORRECT: Broken into stages
void ProcessMessage(...) {
    ValidateMessage(...);  // 20 lines
    AllocateBuffer(...);   // 30 lines
    WriteToLog(...);       // 25 lines
}
```
```

**Benefit:** Makes AI-assisted refactoring tractable.

---

## Comparison: Embarcadero Rules vs Generic AI Coding Rules

| Aspect | Generic Rules | Embarcadero Rules | Why Different? |
|:-------|:--------------|:------------------|:---------------|
| Type hints | Python: Required<br>C++: Optional | C++: Required with @threading, @ownership | Concurrency bugs in distributed systems |
| Alignment | Not mentioned | **CRITICAL**: alignas(64) + static_assert | CXL false sharing = silent corruption |
| Destructors | Mention RAII | **FORBIDDEN**: Manual calls | Actual bug in codebase (E2E test crash) |
| Magic numbers | Discouraged | **FORBIDDEN** with rationale required | Paper spec has many constants (64GB, 4KB, etc.) |
| Comments | Prefer code clarity | **REQUIRED**: Writer annotations on shared fields | Non-cache-coherent memory needs explicit docs |
| Performance | General "be fast" | **REQUIRED**: Mark HOT/COLD, constraints, budgets | Paper claims specific throughput (9GB/s) |

**Key Insight:** Generic rules focus on **readability**. Embarcadero rules focus on **correctness** in a non-cache-coherent distributed system.

---

## Real-World Validation

These rules have **already prevented bugs** in this session:

### Bug 1: Manual Destructor (Caught)
**File:** `src/client/main.cc:256`

**Without rule:**
```cpp
writer.~ResultWriter();  // AI might not flag this
```

**With rule:**
AI immediately knows:
> Rule #5: NEVER manually call destructors
> This causes double-free

**Result:** Bug fixed before commit.

---

### Bug 2: Missing Total Size Parameter (Caught)
**File:** `test/e2e/test_basic_publish.sh`

**Without rule:**
```bash
./throughput_test -m 128  # Missing -s, uses 10GB default
```

**With rule:**
AI checks:
> Rule #12: No magic numbers
> Where is the total_size calculation?

**Result:** Added `-s $((MESSAGE_SIZE * TOTAL_MESSAGES))`

---

### Bug 3: Missing Cache Flush (Would Catch)
**File:** Hypothetical refactoring

**Without rule:**
```cpp
msg_header->received = 1;
// AI doesn't know to add flush
```

**With rule:**
AI enforces:
> Rule #4: All CXL writes MUST be flushed
> Adding: flush_cacheline() + store_fence()

**Result:** Prevents subtle race condition.

---

## Recommendations

### Immediate (Do Now)

1. **Add pre-commit hook** (see template above)
   - Enforces cache alignment
   - Detects manual destructors
   - Warns on missing flushes

2. **Add to 90-rlm-verifier.mdc:**
   ```markdown
   ## EMBARCADERO-SPECIFIC CHECKS
   - If modifying CXL struct: Run `pahole -C StructName build/*.a`
   - If touching hot path: Run performance test baseline
   - If changing concurrency: Update @threading annotations
   ```

### Short-term (This Week)

3. **Create verification script:**
   ```bash
   # scripts/verify_cache_alignment.sh
   # Scans all CXL structs and verifies 64B alignment
   ```

4. **Add complexity budget** (Rule #16 above)

### Long-term (Next Month)

5. **Static analysis integration:**
   - Run `clang-tidy` with custom checks for:
     - Cache-line alignment
     - Missing `volatile` on CXL fields
     - Missing flush after writes

6. **Automated Paper Spec tracking:**
   - Script that scans for `[[PAPER_SPEC: TODO]]` markers
   - Generates progress report

---

## Conclusion

### Answer: Is This Best Practice?

**YES**, this is **excellent** best practice for Embarcadero because:

1. ✅ **Project-specific constraints encoded** (cache alignment, flush requirements)
2. ✅ **Prevents actual bugs** (manual destructors, false sharing)
3. ✅ **Tracks migration progress** (Paper Spec markers)
4. ✅ **Makes AI effective** (context loading, explicit annotations)
5. ✅ **Enables safe refactoring** (ownership docs, thread safety)

### Why It Works

**Generic AI coding rules assume:**
- Cache-coherent memory
- Single host
- No paper spec to match
- No subtle performance constraints

**Embarcadero reality:**
- Non-cache-coherent CXL memory
- Multiple hosts accessing shared memory
- Paper spec with precise algorithms
- Performance claims to validate (9GB/s)

**These rules bridge that gap.**

---

## Adoption Strategy

### Phase 1: Foundation (This Week) ✅ DONE
- [x] Context loader active
- [x] Embarcadero code style documented
- [x] Build verifier active

### Phase 2: Enforcement (Next Week)
- [ ] Add pre-commit hook
- [ ] Create cache alignment verifier script
- [ ] Update RLM verifier with Embarcadero checks

### Phase 3: Automation (This Month)
- [ ] Integrate clang-tidy
- [ ] Paper Spec progress tracker
- [ ] Performance regression detector

---

## For Team Discussion

**Questions to resolve:**

1. **Strictness level:** Should pre-commit hook BLOCK or WARN?
   - Recommendation: BLOCK for alignment, WARN for flushes

2. **Documentation burden:** Is @threading/@ownership too verbose?
   - Recommendation: Required for hot path, optional for cold path

3. **Migration tracking:** Where to put [[PAPER_SPEC]] markers?
   - Recommendation: In code + sync to activeContext.md weekly

---

## Final Grade

| Component | Grade | Notes |
|:----------|:------|:------|
| Context Loader | A+ | Critical for project |
| Code Style Rules | A+ | Project-specific, prevents real bugs |
| Build Verifier | A | Good, could be enhanced |
| **Pre-commit Hook** | **N/A** | **Missing - add this** |
| **Overall** | **A** | **Excellent foundation, add enforcement** |

---

**Recommendation:** Keep all current rules + add pre-commit hook = **A+ rating**

**Last Updated:** 2026-01-24
**Reviewed By:** Systems Architect (Claude)
