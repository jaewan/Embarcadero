# Quick Reference: Specification Governance System

**TL;DR:** AI agents now check `spec_deviation.md` BEFORE `paper_spec.md` when implementing features.

---

## Hierarchy (Check in This Order)

```
1. spec_deviation.md   → Approved improvements (source of truth)
   ↓ (if not mentioned)
2. paper_spec.md       → Reference design (fallback)
   ↓ (if neither specifies)
3. Engineering judgment → Document as new deviation
```

---

## Files Updated

| File | What Changed | Why |
|:-----|:-------------|:----|
| `docs/memory-bank/spec_deviation.md` | **Created** - Deviation tracking | AI knows when to deviate |
| `.cursor/rules/00-context-loader.mdc` | Load deviations FIRST | Establishes hierarchy |
| `.cursor/rules/10-code-style.mdc` | Rule #8 - Deviation policy | AI knows how to propose |
| `docs/memory-bank/paper_spec.md` | Softened header | Reference, not gospel |
| `docs/memory-bank/activeContext.md` | Added current deviations | Session visibility |

---

## Code Markers

### Approved Deviation
```cpp
// [[DEVIATION_001: Batch Size Optimization]]
// See docs/memory-bank/spec_deviation.md DEV-001
size_t batch_size = CalculateAdaptiveBatchSize();
```

### Paper Spec Match
```cpp
// [[PAPER_SPEC: Implemented]] - Matches Table 5
struct alignas(64) BrokerMetadata { ... };
```

### Experimental Proposal
```cpp
// [[DEVIATION_PROPOSAL_003: Zero-Copy Replication]]
// Testing +30% improvement, pending approval
DirectDMA(cxl_addr, disk_fd, size);
```

---

## How AI Uses This

**Implementing a feature:**
1. Read `spec_deviation.md` - deviation documented?
   - YES → Follow deviation (ignore paper)
   - NO → Continue to step 2
2. Read `paper_spec.md` - paper specifies design?
   - YES → Follow paper
   - NO → Use judgment + propose deviation

**Finding better design:**
1. Implement both approaches (paper + proposed)
2. Measure performance difference
3. Add to `spec_deviation.md` with 🔬 status
4. Mark code with `[[DEVIATION_PROPOSAL_XXX]]`
5. Update `activeContext.md` for human review

---

## Current Deviations

- **DEV-001:** Adaptive batch sizing (64KB-4MB) → +9.4% throughput
- **DEV-002:** Batched cache flushes → +15% predicted

---

## Documentation

- **Full guide:** `SPEC_GOVERNANCE_GUIDE.md` (1,200 lines)
- **Summary:** `SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md` (600 lines)
- **Template:** `docs/memory-bank/spec_deviation.md` (220 lines)

---

## Key Rules

1. ✅ **Check deviations FIRST** - Before paper spec
2. ✅ **Document all deviations** - No silent changes
3. ✅ **Quantify improvements** - Measure vs baseline
4. ❌ **Never remove deviations** - To "match paper"
5. ❌ **Never skip documentation** - If deviating

---

**Status:** ✅ Ready for use
**Last Updated:** 2026-01-24
