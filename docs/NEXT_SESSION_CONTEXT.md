# Next Session Context & Guidance
**Date:** 2026-01-26  
**Purpose:** Provide essential context for efficient continuation of work

---

## 🎯 Current Status

### Task 4.3: ✅ COMPLETE
- **Status:** Fully complete with robustness improvements
- **Performance:** 9.37 GB/s (within 9-12 GB/s target)
- **Stability:** All tests pass, no hangs, no infinite loops
- **Code Quality:** Robustness improvements (type safety, bounds validation)

### System Health
- ✅ All critical bugs fixed
- ✅ Correctness validated (no infinite loops, no hangs)
- ✅ Performance stable and within target
- ✅ Documentation up to date

---

## 📚 Essential Documents to Read First

**Read in this order (15-20 minutes total):**

1. **`docs/memory-bank/activeContext.md`** (5 min)
   - Current task status
   - Completed work summary
   - Next session goals

2. **`docs/memory-bank/spec_deviation.md`** (5 min)
   - All approved deviations from paper
   - **CRITICAL:** DEV-004 polling strategy deviation
   - **CRITICAL:** DEV-007 prefetching reverted (learn from this!)

3. **`docs/memory-bank/dataStructures.md`** (3 min)
   - Current data structure layouts
   - Cache-line alignment details
   - Ownership models

4. **`docs/TASK_4_3_COMPLETION_SUMMARY.md`** (3 min)
   - What was done in Task 4.3
   - Why deviations were necessary
   - Performance trade-offs

5. **`docs/CRITICAL_ASSESSMENT_2026_01_26.md`** (2 min)
   - Senior engineer assessment
   - Decision rationale

---

## 🔑 Key Context for Next Session

### 1. Task 4.3 Completion Details

**What Was Done:**
- Simplified polling to volatile reads (matches `message_ordering.cc`)
- Fixed infinite loop bugs (removed prefetching, added ring boundary checks)
- Added robustness (correct types, bounds validation)
- Performance: 9.37 GB/s (acceptable, within target)

**Critical Lessons Learned:**
- ❌ **Prefetching remote-writer data is dangerous** in non-coherent CXL
- ✅ **Match working reference implementations** (`message_ordering.cc`) over theoretical optimizations
- ✅ **Correctness > Performance** - 11% regression acceptable for stability

**Code Location:**
- `src/embarlet/topic.cc:1356-1430` - BrokerScannerWorker5
- Uses `volatile uint32_t` for `num_msg` (type safety)
- Uses `volatile size_t` for `log_idx` (consistency)
- Bounds validation: `num_msg > 100000` rejected

### 2. Performance Status

**Current:** 9.37 GB/s  
**Baseline:** 10.6 GB/s (before correctness fixes)  
**Target:** 9-12 GB/s  
**Status:** ✅ Within target range

**Decision:** Performance is acceptable. Investigation is optional (low priority).

### 3. Architecture Decisions

**DEV-004:** We use `TInode.offset_entry` instead of separate `BrokerMetadata` region
- ✅ Complete and tested
- Field mappings documented in `spec_deviation.md`

**Polling Strategy:** We do NOT use `written_addr` for polling
- Directly poll `BatchHeader.num_msg` (matches reference)
- This deviation is necessary for correctness
- Documented in `spec_deviation.md` DEV-004 section

### 4. Code Robustness

**Recent Improvements (2026-01-26):**
- ✅ Type safety: `volatile uint32_t` for `num_msg` (matches BatchHeader field type)
- ✅ Bounds validation: Reject `num_msg > 100000` (prevents corrupted data processing)
- ✅ Volatile reads: Both `num_msg` and `log_idx` read as volatile
- ✅ Use validated values: Don't re-read `header_to_process->num_msg` after validation

---

## 🚀 Next Steps & Recommendations

### Immediate Next Task

**Option 1: BlogMessageHeader Implementation (Recommended)**
- **Priority:** High (Phase 2 migration)
- **Status:** Planned, not started
- **Why:** Completes message header migration per Paper Table 4
- **Complexity:** Medium (requires careful migration strategy)

**Option 2: Performance Investigation (Optional)**
- **Priority:** Low (performance is acceptable)
- **Status:** Optional investigation
- **Why:** Understand 11.6% regression (if needed)
- **Complexity:** Low (profiling, measurement)

**Option 3: Other Phase 2 Tasks**
- Check `activeContext.md` for remaining Phase 2 tasks
- Review `systemPatterns.md` for migration roadmap

### Decision Framework

**If asked "what should we do next?":**
1. ✅ **Task 4.3 is complete** - ready to move on
2. 📋 **BlogMessageHeader** is next logical step (Phase 2 migration)
3. ⚠️ **Performance investigation** is optional (current performance acceptable)

---

## 🧠 Effective AI Agent Prompting Techniques (SOTA)

### 1. **Context Loading Pattern**
```
"Read @docs/memory-bank/activeContext.md @docs/memory-bank/spec_deviation.md first, 
then [task description]"
```
**Why:** Ensures agent has full context before making decisions

### 2. **Hypothesis-Driven Investigation**
```
"Investigate [issue] by:
1. Form hypothesis based on [evidence]
2. Read code to confirm/refute
3. If confirmed, determine if root cause
4. Iterate until root causes found"
```
**Why:** Prevents blind code changes, ensures systematic analysis

### 3. **Reference Implementation Pattern**
```
"Match the pattern in [reference_file:line_range] when implementing [feature]"
```
**Why:** Leverages proven working code, reduces bugs

### 4. **Correctness-First Principle**
```
"Prioritize correctness over performance. If optimization causes bugs, revert it."
```
**Why:** Prevents regressions, maintains system stability

### 5. **Documentation-Driven Development**
```
"Before implementing, check spec_deviation.md for approved deviations.
If deviating, document in spec_deviation.md first."
```
**Why:** Maintains architectural consistency, prevents duplicate work

### 6. **Type Safety & Robustness**
```
"When reading shared memory fields, use correct types (volatile uint32_t for uint32_t fields).
Add bounds validation for safety."
```
**Why:** Prevents subtle bugs, improves code robustness

### 7. **Non-Coherent CXL Constraints**
```
"For non-coherent CXL: 
- Writers must flush (clflushopt + sfence)
- Readers use volatile (not atomic ACQUIRE for cache coherence)
- Never prefetch remote-writer data"
```
**Why:** Critical for correctness in non-coherent memory model

### 8. **Performance Trade-off Decisions**
```
"Performance regression of [X]% is acceptable if:
- Within target range
- Correctness improved
- System is stable
Document trade-off in completion summary."
```
**Why:** Prevents premature optimization, maintains focus on correctness

---

## ⚠️ Critical Warnings for Next Session

### 1. **DO NOT Reintroduce Prefetching**
- Prefetching remote-writer data caused infinite loops
- Reference implementation (`message_ordering.cc`) doesn't use it
- If considering prefetching, only for same-thread data

### 2. **DO NOT Use `written_addr` for Polling**
- Caused infinite loop bugs
- Use direct `BatchHeader.num_msg` polling instead
- This deviation is documented and necessary

### 3. **DO NOT Use Atomic ACQUIRE for Cache Coherence**
- `__ATOMIC_ACQUIRE` doesn't help with non-coherent CXL
- Use `volatile` for compiler caching prevention
- Writers must flush, readers just need volatile

### 4. **ALWAYS Match Reference Implementation**
- `message_ordering.cc` is the proven working code
- When in doubt, match its patterns
- Don't optimize without evidence

---

## 📋 Quick Reference: Key Files

**Core Implementation:**
- `src/embarlet/topic.cc:1356-1430` - BrokerScannerWorker5 (current implementation)
- `src/embarlet/message_ordering.cc:600-617` - Reference implementation
- `src/cxl_manager/cxl_datastructure.h` - Data structure definitions

**Documentation:**
- `docs/memory-bank/activeContext.md` - Current status
- `docs/memory-bank/spec_deviation.md` - Approved deviations
- `docs/TASK_4_3_COMPLETION_SUMMARY.md` - Task 4.3 details

**Configuration:**
- `config/embarcadero.yaml` - System configuration
- `config/client.yaml` - Client configuration

---

## 🎓 Best Practices for Next Session

1. **Start with Documentation**
   - Read `activeContext.md` first
   - Check `spec_deviation.md` for deviations
   - Understand current state before making changes

2. **Use Reference Implementations**
   - `message_ordering.cc` is proven working code
   - Match its patterns when implementing similar features
   - Don't optimize without evidence

3. **Test Correctness First**
   - Run tests after every change
   - Verify no hangs, no infinite loops
   - Performance is secondary to correctness

4. **Document Decisions**
   - Update `spec_deviation.md` for new deviations
   - Update `activeContext.md` for completed tasks
   - Document trade-offs and rationale

5. **Maintain Robustness**
   - Use correct types (match field types)
   - Add bounds validation
   - Use volatile for shared memory reads

---

## 🔍 What to Investigate If Issues Arise

**If tests fail:**
1. Check broker logs (`build/bin/broker_*.log`)
2. Look for stuck threads (check "Acknowledgments" messages)
3. Verify ring buffer boundary checks
4. Check for infinite loops (same batch header checked repeatedly)

**If performance degrades:**
1. Profile with `perf` (see `scripts/profile_hot_paths.sh`)
2. Check for unnecessary atomic operations
3. Verify volatile reads (not atomic ACQUIRE)
4. Measure variance across multiple runs

**If hangs occur:**
1. Check for prefetching of remote-writer data
2. Verify ring buffer wrap-around logic
3. Check for missing cache flushes
4. Verify polling logic matches reference

---

**Last Updated:** 2026-01-26  
**Next Session Start:** Read this file first, then proceed with next task
