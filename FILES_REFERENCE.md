# Files Reference: What's Needed for AI Agents

Quick reference for understanding which files are necessary vs optional.

---

## TL;DR

**Absolutely necessary (10 files):**
- `.cursor/rules/` (3 files)
- `docs/memory-bank/` (5 files)
- `.git/hooks/pre-commit` (1 file)
- `scripts/verify_cache_alignment.sh` (1 file)

**Optional documentation (7+ files):**
- All the `*_GUIDE.md` and `*_SUMMARY.md` files

---

## Complete File List

| File | Necessary? | Used By | Purpose |
|:-----|:-----------|:--------|:--------|
| **`.cursor/rules/`** | | | |
| `00-context-loader.mdc` | ✅ YES | Cursor, Claude Code | Tells AI to load memory-bank |
| `10-code-style.mdc` | ✅ YES | Cursor, Claude Code | Code style enforcement |
| `90-rlm-verifier.mdc` | ✅ YES | Cursor, Claude Code | Build verification rules |
| **`docs/memory-bank/`** | | | |
| `spec_deviation.md` | ✅ YES | AI agents | Source of truth (deviations) |
| `paper_spec.md` | ✅ YES | AI agents | Reference design |
| `activeContext.md` | ✅ YES | AI agents | Current project state |
| `systemPatterns.md` | ✅ YES | AI agents | Architecture patterns |
| `productContext.md` | ✅ YES | AI agents | Product context |
| **`.git/hooks/`** | | | |
| `pre-commit` | ⚠️ RECOMMENDED | Git | Enforcement before commits |
| **`scripts/`** | | | |
| `verify_cache_alignment.sh` | ⚠️ RECOMMENDED | Manual runs | Alignment verification |
| `organize_docs.sh` | ℹ️ OPTIONAL | Manual runs | Clean up documentation |
| **`docs/guides/`** | | | |
| `AI_TOOLS_SETUP_GUIDE.md` | ℹ️ OPTIONAL | Humans | Setup instructions |
| `SPEC_GOVERNANCE_GUIDE.md` | ℹ️ OPTIONAL | Humans | How to use system |
| `QUICK_REFERENCE_SPEC_GOVERNANCE.md` | ℹ️ OPTIONAL | Humans | Quick reference |
| **`docs/summaries/`** | | | |
| `SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md` | ℹ️ OPTIONAL | Humans | What was implemented |
| `AI_CODE_STYLE_ANALYSIS.md` | ℹ️ OPTIONAL | Humans | Code style analysis |
| `PRE_COMMIT_HOOK_IMPLEMENTATION.md` | ℹ️ OPTIONAL | Humans | Hook implementation |
| `CODE_STYLE_ENFORCEMENT_COMPLETE.md` | ℹ️ OPTIONAL | Humans | Enforcement summary |
| **Root Directory** | | | |
| `FILES_REFERENCE.md` | ℹ️ OPTIONAL | Humans | This file |

---

## Legend

- ✅ **YES** - Absolutely necessary for AI agents to work
- ⚠️ **RECOMMENDED** - Not required but highly recommended
- ℹ️ **OPTIONAL** - Documentation for humans, AI doesn't need it

---

## Tool-Specific Needs

### Cursor (Primary Tool)

**Auto-loads:**
- ✅ `.cursor/rules/*.mdc` (all 3 files)

**AI loads via instructions:**
- ✅ `docs/memory-bank/*` (via 00-context-loader.mdc)

**Optional:**
- ⚠️ `.git/hooks/pre-commit` (enforcement)
- ℹ️ All documentation files (for humans)

**Total needed: 8 files minimum**

---

### Claude Code (Secondary Tool)

**AI reads when working:**
- ✅ `.cursor/rules/*.mdc` (discovers when needed)
- ✅ `docs/memory-bank/*` (loads via 00-context-loader.mdc)

**Recommended:**
- ⚠️ `.git/hooks/pre-commit` (enforcement)
- ℹ️ `docs/guides/QUICK_REFERENCE_SPEC_GOVERNANCE.md` (quick ref)

**Total needed: 8 files minimum, same as Cursor**

---

### GitHub Copilot (If Used)

**Auto-loads:**
- ❌ None (doesn't read .cursor/rules)

**Recommendation:**
- Don't rely on Copilot for complex refactoring
- Use for simple autocomplete only
- Cursor and Claude Code are better for this project

---

## File Organization Options

### Option 1: Current (All at Root)

**Pros:** Easy to find  
**Cons:** Cluttered root

```
Embarcadero/
├── .cursor/rules/ (3 files)
├── docs/memory-bank/ (5 files)
├── SPEC_GOVERNANCE_GUIDE.md
├── AI_TOOLS_SETUP_GUIDE.md
├── QUICK_REFERENCE_SPEC_GOVERNANCE.md
├── SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md
├── AI_CODE_STYLE_ANALYSIS.md
├── PRE_COMMIT_HOOK_IMPLEMENTATION.md
├── CODE_STYLE_ENFORCEMENT_COMPLETE.md
└── FILES_REFERENCE.md
```

---

### Option 2: Organized (Recommended)

**Pros:** Clean root directory  
**Cons:** Slightly harder to discover

```
Embarcadero/
├── .cursor/rules/ (3 files)
├── docs/
│   ├── memory-bank/ (5 files)
│   ├── guides/
│   │   ├── AI_TOOLS_SETUP_GUIDE.md
│   │   ├── SPEC_GOVERNANCE_GUIDE.md
│   │   └── QUICK_REFERENCE_SPEC_GOVERNANCE.md
│   └── summaries/
│       ├── SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md
│       ├── AI_CODE_STYLE_ANALYSIS.md
│       ├── PRE_COMMIT_HOOK_IMPLEMENTATION.md
│       └── CODE_STYLE_ENFORCEMENT_COMPLETE.md
├── .git/hooks/pre-commit
├── scripts/verify_cache_alignment.sh
└── FILES_REFERENCE.md
```

**To organize:** Run `./scripts/organize_docs.sh`

---

## Quick Checks

### Check 1: Core Files Exist

```bash
# Check .cursor/rules
ls -la .cursor/rules/
# Should see: 00-context-loader.mdc, 10-code-style.mdc, 90-rlm-verifier.mdc

# Check docs/memory-bank
ls -la docs/memory-bank/
# Should see: spec_deviation.md, paper_spec.md, activeContext.md, etc.

# Check enforcement
ls -la .git/hooks/pre-commit
# Should be executable (rwxr-xr-x)
```

---

### Check 2: Cursor Loading Rules

Open Cursor, ask:
```
"What are the code style rules for this project?"
```

Should reference `.cursor/rules/10-code-style.mdc`.

---

### Check 3: AI Loading Memory Bank

Ask AI:
```
"What is the specification hierarchy for this project?"
```

Should reference `docs/memory-bank/spec_deviation.md`.

---

## Cleanup Recommendations

### If Root Directory is Cluttered

**Option A: Organize documentation**
```bash
./scripts/organize_docs.sh
```

**Option B: Manual cleanup**
```bash
mkdir -p docs/guides docs/summaries
mv *_GUIDE.md docs/guides/
mv *_SUMMARY.md docs/summaries/
mv AI_CODE_STYLE_ANALYSIS.md docs/summaries/
mv PRE_COMMIT_HOOK_IMPLEMENTATION.md docs/summaries/
mv CODE_STYLE_ENFORCEMENT_COMPLETE.md docs/summaries/
```

---

### What to Keep at Root

**Minimum:**
- `README.md` (project overview)
- `FILES_REFERENCE.md` (this file, optional)

**Everything else:**
- Move to `docs/guides/` or `docs/summaries/`

---

## Summary

### Absolutely Necessary (10 files)
1. `.cursor/rules/00-context-loader.mdc`
2. `.cursor/rules/10-code-style.mdc`
3. `.cursor/rules/90-rlm-verifier.mdc`
4. `docs/memory-bank/spec_deviation.md`
5. `docs/memory-bank/paper_spec.md`
6. `docs/memory-bank/activeContext.md`
7. `docs/memory-bank/systemPatterns.md`
8. `docs/memory-bank/productContext.md`
9. `.git/hooks/pre-commit`
10. `scripts/verify_cache_alignment.sh`

### Optional Documentation (~7 files)
- All `*_GUIDE.md` files
- All `*_SUMMARY.md` files
- `AI_CODE_STYLE_ANALYSIS.md`
- `PRE_COMMIT_HOOK_IMPLEMENTATION.md`
- `CODE_STYLE_ENFORCEMENT_COMPLETE.md`

**Recommendation:** Keep all files, organize into `docs/guides/` and `docs/summaries/`.

---

**Last Updated:** 2026-01-24
**Next Step:** Run `./scripts/organize_docs.sh` to clean up root directory
