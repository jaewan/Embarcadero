# Specification Governance Guide for AI-Assisted Development

**Purpose:** How to refactor Embarcadero with AI agents when better designs exist than the paper
**Date:** 2026-01-24
**Status:** ✅ Active

---

## Executive Summary

### The Problem

When refactoring Embarcadero to align with the NSDI '26 paper, we sometimes discover **better designs** than what the paper describes. How do we:

1. Allow AI agents to implement improvements?
2. Track what differs from the paper and why?
3. Prevent AI from silently deviating without documentation?
4. Maintain paper as reference while evolving beyond it?

### The Solution

**Three-tier specification hierarchy:**

```
┌────────────────────────────────────────┐
│ 1. spec_deviation.md                   │
│    (Approved improvements)             │
│    → SOURCE OF TRUTH when present      │
└────────────────────────────────────────┘
                 ↓
      (if not mentioned, fall back to...)
                 ↓
┌────────────────────────────────────────┐
│ 2. paper_spec.md                       │
│    (Reference design)                  │
│    → Use if no deviation documented    │
└────────────────────────────────────────┘
                 ↓
      (if neither specifies...)
                 ↓
┌────────────────────────────────────────┐
│ 3. Engineering Judgment                │
│    → Document as deviation proposal    │
└────────────────────────────────────────┘
```

**Result:** AI agents can propose and implement improvements while maintaining traceable governance.

---

## Quick Start for AI Agents

### When implementing a feature:

```
1. Read spec_deviation.md
   ├─ Is this design mentioned?
   │  ├─ YES → Follow the deviation (ignore paper)
   │  └─ NO  → Go to step 2
   │
2. Read paper_spec.md
   ├─ Is this design specified?
   │  ├─ YES → Follow the paper design
   │  └─ NO  → Go to step 3
   │
3. Use engineering judgment
   └─ Document your choice as deviation proposal
```

### Example: Implementing Batch Processing

**Step 1: Check spec_deviation.md**
```markdown
## DEV-001: Batch Size Optimization
Status: 🔬 Experimental
What We Do: Adaptive batch size 64KB - 4MB
```

**Decision:** Use adaptive batch size (deviation documented)
**Ignore:** Paper's fixed 512KB batch size

**Step 2: Mark in code**
```cpp
// [[DEVIATION_001: Batch Size Optimization]]
// See docs/memory-bank/spec_deviation.md DEV-001
// Paper uses fixed 512KB, we use adaptive 64KB-4MB
size_t batch_size = CalculateAdaptiveBatchSize();
```

---

## Document Structure

### 1. spec_deviation.md

**Purpose:** Source of truth for approved improvements
**Authority:** Overrides paper_spec.md
**Location:** `docs/memory-bank/spec_deviation.md`

**Structure:**
```markdown
## DEV-XXX: [Deviation Name]

Status: ✅ | 🚧 | 📋 | 🔬
Category: Performance | Correctness | Maintainability | Hardware
Impact: Critical | High | Medium | Low

### What Paper Says:
[Paper's design]

### What We Do Instead:
[Our implementation]

### Why It's Better:
- Performance: +X%
- Simplicity: ...
- Correctness: ...

### Performance Impact:
- Baseline: Y GB/s
- Ours: Z GB/s
- Improvement: +X%

### Risks & Mitigation:
[What could go wrong and how we handle it]

### Implementation Notes:
- Files: [List]
- Markers: [[DEVIATION_XXX]]
- Tests: [Coverage]

### Revert Conditions:
[When to go back to paper design]
```

**Status Values:**
- ✅ **Implemented** - Code matches this deviation
- 🚧 **In Progress** - Currently implementing
- 📋 **Planned** - Approved but not started
- 🔬 **Experimental** - Testing if better, may revert

---

### 2. paper_spec.md

**Purpose:** Reference design from NSDI '26 paper
**Authority:** Fallback when spec_deviation.md doesn't specify
**Location:** `docs/memory-bank/paper_spec.md`

**Usage:**
- ✅ Read to understand baseline architecture
- ✅ Use as reference for comparisons
- ✅ Follow if no deviation documented
- ❌ Don't blindly follow if better design exists
- ❌ Don't modify (keep as immutable reference)

**Updated Header:**
```markdown
# Technical Specification: Embarcadero Reference Design

**Authority:** Reference design - Check spec_deviation.md FIRST
**Usage:** Follow this IF no deviation documented

⚠️ IMPORTANT: Specification Hierarchy
1. spec_deviation.md - CHECK THIS FIRST
2. paper_spec.md (THIS FILE)
3. Engineering judgment
```

---

### 3. .cursor/rules Updates

#### A. 00-context-loader.mdc

**Added:**
```markdown
## 1. CORE MEMORY

### Priority 1: Specification Hierarchy (CHECK IN THIS ORDER)
- docs/memory-bank/spec_deviation.md - Approved improvements
- docs/memory-bank/paper_spec.md - Reference design

### Priority 2: Project Context
- docs/memory-bank/productContext.md
- docs/memory-bank/activeContext.md
- docs/memory-bank/systemPatterns.md
```

**Effect:** AI loads deviation file BEFORE paper spec

---

#### B. 10-code-style.mdc Rule #8

**Updated:**
```markdown
## 8. SPECIFICATION COMPLIANCE & DEVIATIONS

### Rule: Follow Specification Hierarchy

CRITICAL: Check in this order:
1. spec_deviation.md - Approved improvements
2. paper_spec.md - Reference design
3. Engineering judgment - Document as proposal

### Markers:

// [[DEVIATION_XXX: Name]]
// See spec_deviation.md DEV-XXX
[Code implementing deviation]

// [[PAPER_SPEC: Implemented]]
[Code matching paper exactly]

// [[DEVIATION_PROPOSAL_XXX: Name]]
// Experimental - needs approval
[Code with proposed improvement]
```

**Effect:** AI knows when deviations are allowed and how to propose new ones

---

#### C. 90-rlm-verifier.mdc

**Added:**
```markdown
### When Implementing Paper Spec:
- Check spec_deviation.md first
- Mark status: [[DEVIATION_XXX]] or [[PAPER_SPEC: Implemented]]
- Update activeContext.md with progress
```

**Effect:** Build verification checks spec hierarchy

---

### 4. activeContext.md

**Added:**
```markdown
## ⚠️ Specification Governance

CRITICAL: Check in this order:
1. spec_deviation.md - Approved improvements
2. paper_spec.md - Reference design
3. Engineering judgment - Document as proposal

**Active Deviations:**
- DEV-001: Batch Size Optimization - 🔬 Experimental
- DEV-002: Cache Flush Optimization - 📋 Planned
```

**Effect:** Current session state shows active deviations

---

## Code Markers

### Marker Types

#### 1. Approved Deviation
```cpp
// [[DEVIATION_001: Batch Size Optimization]]
// See docs/memory-bank/spec_deviation.md DEV-001
// Paper uses fixed 512KB, we use adaptive 64KB-4MB
size_t batch_size = CalculateAdaptiveBatchSize();
```

**When:** Deviation is documented in spec_deviation.md
**Search:** `git grep "DEVIATION_001"`

---

#### 2. Paper Spec Implemented
```cpp
// [[PAPER_SPEC: Implemented]] - Matches Table 5 exactly
struct alignas(64) BrokerMetadata {
    volatile uint64_t log_ptr;        // [[WRITER: Broker]]
    volatile uint64_t processed_ptr;  // [[WRITER: Broker]]
};
static_assert(sizeof(BrokerMetadata) == 64, "Must be 64B");
```

**When:** Code exactly matches paper specification
**Search:** `git grep "PAPER_SPEC: Implemented"`

---

#### 3. Paper Spec TODO
```cpp
// [[PAPER_SPEC: TODO]] - Need to migrate to Bmeta
struct TInode {
    // Old monolithic structure, will replace with BrokerMetadata
};
```

**When:** Old design that needs migration to paper spec
**Search:** `git grep "PAPER_SPEC: TODO"`

---

#### 4. Deviation Proposal (Experimental)
```cpp
// [[DEVIATION_PROPOSAL_003: Zero-Copy Batch Transfer]]
// Experimental - 20% faster than paper's approach
// If approved, move to spec_deviation.md as DEV-003
void TransferBatch() {
    // New approach using RDMA directly
}
```

**When:** Experimenting with improvement, not yet approved
**Search:** `git grep "DEVIATION_PROPOSAL"`
**Action:** After validation, move to spec_deviation.md with status 🔬

---

## Workflow: Proposing a Deviation

### Step 1: Identify Opportunity

**Scenario:** AI is implementing cache flush logic, finds paper approach inefficient

```cpp
// Paper approach (flush after every field write):
msg_header->field1 = value1;
CXL::flush_cacheline(msg_header);
CXL::store_fence();

msg_header->field2 = value2;
CXL::flush_cacheline(msg_header);  // Redundant!
CXL::store_fence();
```

**Better approach:** Batch flushes within same cache line

---

### Step 2: Implement Both Approaches

**Baseline (Paper):**
```cpp
void UpdateHeaderPaper(MessageHeader* hdr) {
    hdr->size = 1024;
    CXL::flush_cacheline(hdr);
    CXL::store_fence();

    hdr->received = 1;
    CXL::flush_cacheline(hdr);  // Same cache line!
    CXL::store_fence();
}
```

**Proposed (Batched):**
```cpp
// [[DEVIATION_PROPOSAL_002: Cache Flush Batching]]
void UpdateHeaderBatched(MessageHeader* hdr) {
    hdr->size = 1024;
    hdr->received = 1;
    // Single flush for all fields in same cache line
    CXL::flush_cacheline(hdr);
    CXL::store_fence();
}
```

---

### Step 3: Measure Performance

**Benchmark:**
```bash
# Baseline
./throughput_test --flush-mode=paper
# Result: 8.5 GB/s

# Proposed
./throughput_test --flush-mode=batched
# Result: 9.8 GB/s

# Improvement: +15.3%
```

---

### Step 4: Document in spec_deviation.md

**Add entry:**
```markdown
## DEV-002: Cache Flush Optimization

**Status:** 🔬 Experimental
**Category:** Performance
**Impact:** Medium
**Date Approved:** 2026-01-24

### What Paper Says:
- Flush every cache line after write (§4.2)
- Pattern: `clflushopt(ptr); sfence();` after each field

### What We Do Instead:
- Batch flushes within same 64-byte region
- Write all fields → flush once → fence once

### Why It's Better:
- **Reduced flush overhead:** Paper flushes N times, we flush once
- **Fewer serialization points:** Better CPU pipeline utilization
- **Same correctness:** All writes flushed before fence

### Performance Impact:
- **Baseline (paper):** 8.5 GB/s
- **Our implementation:** 9.8 GB/s
- **Improvement:** +15.3%

### Risks & Mitigation:
- **Risk:** Incorrect flush placement → stale reads
- **Mitigation:** Static analysis, extensive testing

### Implementation Notes:
- **Files:** `src/embarlet/topic.cc`, `src/cxl_manager/cxl_manager.cc`
- **Markers:** `[[DEVIATION_002]]`
- **Tests:** `test/e2e/test_cache_coherence.sh`

### Revert Conditions:
- If any test shows stale data
- If performance improvement < 5%
```

---

### Step 5: Update Code Markers

**Change from proposal to approved:**
```cpp
// [[DEVIATION_002: Cache Flush Optimization]]
// See docs/memory-bank/spec_deviation.md DEV-002
// Paper flushes per field, we batch flushes per cache line
void UpdateHeader(MessageHeader* hdr) {
    hdr->size = 1024;
    hdr->received = 1;
    CXL::flush_cacheline(hdr);  // Single flush
    CXL::store_fence();
}
```

---

### Step 6: Update activeContext.md

**Add to current deviations:**
```markdown
**Active Deviations:**
- DEV-001: Batch Size Optimization - 🔬 Experimental - +9.4% throughput
- DEV-002: Cache Flush Optimization - 🔬 Experimental - +15.3% throughput
```

---

## AI Agent Instructions

### When Starting a Task

```python
def implement_feature(feature_name):
    # Step 1: Load specification hierarchy
    deviations = read("docs/memory-bank/spec_deviation.md")
    paper_spec = read("docs/memory-bank/paper_spec.md")

    # Step 2: Check deviations first
    if feature_name in deviations:
        return implement_deviation(deviations[feature_name])

    # Step 3: Fall back to paper spec
    if feature_name in paper_spec:
        return implement_paper_design(paper_spec[feature_name])

    # Step 4: Use engineering judgment
    return propose_new_design(feature_name)
```

---

### When Reading Code

**Pattern 1: Deviation marker**
```cpp
// [[DEVIATION_001: Batch Size Optimization]]
```

**Action:**
1. Look up DEV-001 in spec_deviation.md
2. Understand why this differs from paper
3. Preserve the deviation in refactoring

---

**Pattern 2: Paper spec marker**
```cpp
// [[PAPER_SPEC: Implemented]]
```

**Action:**
1. This code matches paper exactly
2. Check spec_deviation.md - is there a better approach?
3. If yes, propose migration to deviation

---

**Pattern 3: Proposal marker**
```cpp
// [[DEVIATION_PROPOSAL_003: Better Algorithm]]
```

**Action:**
1. Experimental code, not yet approved
2. Check if performance validates the improvement
3. If yes, move to spec_deviation.md with 🔬 status

---

### When Refactoring

**Scenario:** Refactoring message processing pipeline

**Step 1: Identify affected components**
- Message header updates
- Cache flush logic
- Batch processing

**Step 2: Check deviations for each component**
```bash
# Check what deviations exist
grep "DEV-" docs/memory-bank/spec_deviation.md

# Found:
# DEV-001: Batch Size (adaptive)
# DEV-002: Cache Flush (batched)
```

**Step 3: Implement according to deviations**
```cpp
// Use adaptive batch size (DEV-001)
size_t batch_size = CalculateAdaptiveBatchSize();

// Use batched flushes (DEV-002)
UpdateAllFields(hdr);
CXL::flush_cacheline(hdr);  // Single flush
```

**Step 4: Preserve markers**
```cpp
// [[DEVIATION_001: Batch Size Optimization]]
// [[DEVIATION_002: Cache Flush Optimization]]
```

---

## Governance Policies

### Approval Criteria

#### Performance Deviations
- **Threshold:** >10% improvement
- **Required:** Benchmark data comparing paper vs deviation
- **Review:** Weekly for 🔬 Experimental status
- **Approval:** Engineering lead sign-off

#### Correctness Deviations
- **Threshold:** Fixes bug or correctness issue in paper
- **Required:** Proof that paper design has flaw
- **Review:** Immediate
- **Approval:** Team consensus

#### Maintainability Deviations
- **Threshold:** No performance regression
- **Required:** Clear readability/testability benefit
- **Review:** During code review
- **Approval:** 2+ engineer approval

#### Hardware Constraint Deviations
- **Threshold:** Paper assumes unavailable hardware
- **Required:** Document hardware difference
- **Review:** When hardware changes
- **Approval:** Architecture review

---

### Status Transitions

```
📋 Planned
  ↓ (implementation starts)
🚧 In Progress
  ↓ (code complete, testing)
🔬 Experimental
  ↓ (validation passes)
✅ Implemented
```

**OR**

```
🔬 Experimental
  ↓ (issues found)
❌ Reverted
```

---

### Review Schedule

- **Daily:** Review all 🚧 In Progress deviations
- **Weekly:** Review all 🔬 Experimental deviations (keep/revert decision)
- **Monthly:** Review all ✅ Implemented deviations (still optimal?)
- **Quarterly:** Review all 📋 Planned deviations (still needed?)

---

## Examples

### Example 1: Implementing New Feature

**Task:** Implement global ordering (sequencer)

**Step 1:** Check spec_deviation.md
```markdown
# No deviation for sequencer algorithm
```

**Step 2:** Check paper_spec.md
```markdown
## Stage 3: Global Ordering (Sequencer)

1. Poll: Read Bmeta.processed_ptr
2. Validate FIFO: Check batch seqno
3. CAS Update: CAS(&next_batch_seqno[client_id], ...)
4. Global Order: Write total_order
5. Commit: Update Bmeta.ordered_ptr
```

**Step 3:** Implement paper design
```cpp
// [[PAPER_SPEC: Implemented]] - Matches §3 Algorithm
void GlobalOrderingThread() {
    // Step 1: Poll
    uint64_t processed = ReadProcessedPtr(broker_id);

    // Step 2: Validate FIFO
    if (!ValidateBatchSeqno(client_id, batch)) return;

    // Step 3: CAS Update
    uint64_t expected = next_batch_seqno_[client_id];
    if (!CAS(&next_batch_seqno_[client_id], expected, expected + 1))
        return;

    // Step 4: Global Order
    msg_header->total_order = global_seqno_++;

    // Step 5: Commit
    UpdateOrderedPtr(broker_id, new_ptr);
}
```

**Result:** Follows paper exactly (no deviation needed)

---

### Example 2: Discovering Better Design

**Task:** Implement batch processing

**Step 1:** Check spec_deviation.md
```markdown
## DEV-001: Batch Size Optimization
Status: 🔬 Experimental
What We Do: Adaptive 64KB - 4MB
```

**Step 2:** Implement deviation (ignore paper)
```cpp
// [[DEVIATION_001: Batch Size Optimization]]
// See docs/memory-bank/spec_deviation.md DEV-001
// Paper uses fixed 512KB, we use adaptive batching
void ProcessBatch() {
    size_t batch_size = CalculateAdaptiveBatchSize(
        network_util,
        queue_depth,
        latency_target
    );
    // ... use adaptive batch_size
}
```

**Result:** Uses approved deviation (better than paper)

---

### Example 3: Proposing New Deviation

**Task:** Implementing replication, notice inefficiency

**Current (Paper):**
```cpp
// Paper: Pull from Primary CXL, write to local disk
void ReplicateMessages() {
    // Step 1: Read from CXL
    void* data = ReadFromPrimaryCXL(offset, size);

    // Step 2: Copy to local buffer
    void* buffer = malloc(size);
    memcpy(buffer, data, size);  // Extra copy!

    // Step 3: Write to disk
    WriteToDisk(buffer, size);
    free(buffer);
}
```

**Better approach:**
```cpp
// [[DEVIATION_PROPOSAL_004: Zero-Copy Replication]]
// Eliminate intermediate buffer, write CXL data directly to disk
void ReplicateMessages() {
    // Direct DMA from CXL to disk (zero-copy)
    DirectDMA(primary_cxl_addr, disk_fd, size);
}
```

**Next steps:**
1. Benchmark both approaches
2. Measure improvement (expect +30% throughput)
3. Add to spec_deviation.md as DEV-004 with 🔬 status
4. Update markers from PROPOSAL to DEVIATION_004
5. Add to activeContext.md for team review

---

## Common Pitfalls

### ❌ Pitfall 1: Silent Deviation

**Wrong:**
```cpp
// Paper says batch size 512KB, but I'll use 1MB
size_t batch_size = 1024 * 1024;  // No documentation!
```

**Correct:**
```cpp
// [[DEVIATION_PROPOSAL_005: Larger Batch Size]]
// Testing 1MB batches vs paper's 512KB
// If validated, add to spec_deviation.md
size_t batch_size = 1024 * 1024;
```

---

### ❌ Pitfall 2: Removing Deviation to "Match Paper"

**Wrong:**
```cpp
// "Cleaning up" by removing deviation to match paper
size_t batch_size = 512 * 1024;  // ❌ Lost the improvement!
```

**Correct:**
```cpp
// [[DEVIATION_001: Batch Size Optimization]]
// Keep the deviation, it's better than paper
size_t batch_size = CalculateAdaptiveBatchSize();
```

---

### ❌ Pitfall 3: Not Updating spec_deviation.md

**Wrong:**
```cpp
// Code has deviation marker
// [[DEVIATION_001: Batch Size]]
// But spec_deviation.md doesn't document it!
```

**Correct:**
```markdown
# In spec_deviation.md:
## DEV-001: Batch Size Optimization
Status: ✅ Implemented
[Full documentation...]
```

---

## Tools & Scripts

### Check Deviation Coverage

**Script:** `scripts/check_deviation_markers.sh`
```bash
#!/bin/bash
# Find all deviation markers in code
grep -r "DEVIATION_[0-9]" src/ | while read line; do
    dev_id=$(echo "$line" | grep -oP "DEVIATION_\K[0-9]+")

    # Check if documented in spec_deviation.md
    if ! grep -q "DEV-$dev_id" docs/memory-bank/spec_deviation.md; then
        echo "WARNING: DEVIATION_$dev_id not documented!"
    fi
done
```

---

### Generate Deviation Report

**Script:** `scripts/deviation_report.sh`
```bash
#!/bin/bash
# Generate report of all deviations
echo "# Deviation Status Report"
echo ""
grep "^## DEV-" docs/memory-bank/spec_deviation.md | while read line; do
    dev_id=$(echo "$line" | grep -oP "DEV-\K[0-9]+")
    status=$(grep -A2 "^## $line" docs/memory-bank/spec_deviation.md | grep "Status:" | cut -d: -f2)
    echo "- DEV-$dev_id: $status"
done
```

---

## Summary

### What We Built

1. **spec_deviation.md** - Source of truth for approved improvements
2. **Updated paper_spec.md** - Reference design (not absolute truth)
3. **Updated .cursor/rules/** - AI knows the hierarchy
4. **Updated activeContext.md** - Current deviations visible
5. **Governance guide** - This document

### How It Works

```
AI receives task
  ↓
Loads spec_deviation.md FIRST
  ↓
Checks if deviation exists
  ├─ YES → Follow deviation
  └─ NO  → Check paper_spec.md
          ├─ YES → Follow paper
          └─ NO  → Use judgment + propose deviation
```

### Result

- ✅ AI can implement improvements beyond paper
- ✅ All deviations documented and tracked
- ✅ Paper remains as reference
- ✅ Clear governance process
- ✅ No silent deviations

---

**Status:** ✅ Complete
**Last Updated:** 2026-01-24
**Maintained By:** Engineering Team
