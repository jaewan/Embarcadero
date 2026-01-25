# AI Tools Setup Guide: Cursor vs Claude Code

**Question:** What documents are absolutely necessary for AI agents?
**Answer:** Different tools need different setups.

---

## Quick Answer

### Absolutely Necessary (Core Rules)
```
.cursor/rules/
├── 00-context-loader.mdc   ← Tells AI what to load
├── 10-code-style.mdc       ← Code style enforcement
└── 90-rlm-verifier.mdc     ← Build verification

docs/memory-bank/
├── spec_deviation.md       ← Source of truth (deviations)
├── paper_spec.md           ← Reference design
├── activeContext.md        ← Current state
├── systemPatterns.md       ← Architecture
└── productContext.md       ← Product context
```

### Optional (Human Documentation)
```
SPEC_GOVERNANCE_GUIDE.md                    ← How to use the system
SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md    ← What was implemented
QUICK_REFERENCE_SPEC_GOVERNANCE.md          ← One-page reference
AI_CODE_STYLE_ANALYSIS.md                   ← Analysis
PRE_COMMIT_HOOK_IMPLEMENTATION.md           ← Hook details
CODE_STYLE_ENFORCEMENT_COMPLETE.md          ← Summary
```

**Recommendation:** Keep all files, archive summaries in `docs/` if desired.

---

## How Each Tool Works

### 1. Cursor

#### What Cursor Does Automatically ✅

**YES**, Cursor automatically:
1. ✅ Reads **all** `.cursor/rules/*.mdc` files
2. ✅ Applies rules based on `globs` pattern matching
3. ✅ Includes rule content in AI context when working on matching files
4. ✅ Uses `alwaysApply: true` to apply rules globally

**Example:**
```yaml
---
description: GLOBAL CONTEXT INJECTION
globs: *
alwaysApply: true
---
```
↓
Cursor loads this rule for **every file** you edit.

#### What Cursor Does NOT Do Automatically ❌

**NO**, Cursor does NOT:
❌ Automatically read `docs/memory-bank/` files
❌ Follow the instructions in `00-context-loader.mdc` to load those files
❌ Read the "summary" documentation files

**Why?** Cursor loads the **rules**, but relies on the AI (Claude/GPT-4) to:
1. Read the rule content
2. See the instruction to load memory-bank files
3. Actually use the Read tool to load those files

**Result:** The AI **should** load memory-bank files when it reads 00-context-loader.mdc, but it depends on the AI following instructions.

#### How to Verify Cursor is Working

**Test:**
1. Open a `.cc` file in Cursor
2. Ask: "What are the cache-line alignment requirements?"
3. Expected: AI should reference Rule #2 from 10-code-style.mdc

**If AI doesn't reference rules:**
- Check `.cursor/rules/*.mdc` files exist
- Check YAML frontmatter is valid
- Try restarting Cursor

---

### 2. Claude Code (This CLI Tool)

#### What Claude Code Does Automatically ✅

**NO**, Claude Code does NOT automatically read `.cursor/rules/`.

Claude Code is a CLI tool that:
- Operates from terminal
- Has its own configuration system
- Can be configured to load files at startup

#### How to Configure Claude Code to Use Rules

**Option A: Manual Loading (Current Behavior)**

Claude Code relies on:
1. User starting conversation
2. AI reading `.cursor/rules/00-context-loader.mdc` 
3. AI then loading memory-bank files

**Option B: Startup Configuration (Recommended)**

Create a startup script that loads core context:

**File:** `.claude/startup.sh`
```bash
#!/bin/bash
# Claude Code startup - load core context

cat << 'EOFCONTEXT'
# CORE CONTEXT LOADED

## Specification Hierarchy
1. Check: docs/memory-bank/spec_deviation.md (approved deviations)
2. Fallback: docs/memory-bank/paper_spec.md (reference design)

## Current State
See: docs/memory-bank/activeContext.md

## Code Style
See: .cursor/rules/10-code-style.mdc

## Architecture
See: docs/memory-bank/systemPatterns.md
EOFCONTEXT
```

**Option C: MCP Server (Advanced)**

Configure Claude Code with MCP (Model Context Protocol) server:
- MCP server can inject memory-bank content automatically
- Requires MCP server setup
- Beyond scope of this guide

#### Recommended: Use Cursor Rules as Reference

Since Claude Code doesn't auto-load `.cursor/rules/`, the **current approach works**:
1. `.cursor/rules/00-context-loader.mdc` instructs AI to load files
2. AI reads the rule (when working)
3. AI loads memory-bank files

**This works because:** Claude Code still sees `.cursor/rules/` files when searching the codebase, and AI agents (like me) will read them when relevant.

---

### 3. Windsurf

**Status:** Unknown (not tested)

Likely similar to Cursor:
- May have its own rules directory (`.windsurf/rules/`?)
- Check Windsurf documentation

**Recommendation:** If using Windsurf, copy `.cursor/rules/` to `.windsurf/rules/` and test.

---

## What You Actually Need

### Minimal Setup (Cursor Only)

**Essential Files:**
```
.cursor/rules/
├── 00-context-loader.mdc   # Instructs AI to load memory-bank
├── 10-code-style.mdc       # Code style rules
└── 90-rlm-verifier.mdc     # Build verification

docs/memory-bank/
├── spec_deviation.md       # Deviations from paper
├── paper_spec.md           # Reference design
├── activeContext.md        # Current state
├── systemPatterns.md       # Architecture patterns
└── productContext.md       # Product context

.git/hooks/
└── pre-commit              # Enforcement (optional but recommended)

scripts/
└── verify_cache_alignment.sh  # Verification (optional)
```

**Total: 10 files** (core system)

### Full Setup (Cursor + Claude Code)

**Add to minimal:**
```
docs/guides/  (new directory)
├── SPEC_GOVERNANCE_GUIDE.md
├── SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md
└── QUICK_REFERENCE_SPEC_GOVERNANCE.md

# Or keep at root:
SPEC_GOVERNANCE_GUIDE.md
SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md  
QUICK_REFERENCE_SPEC_GOVERNANCE.md
AI_CODE_STYLE_ANALYSIS.md
PRE_COMMIT_HOOK_IMPLEMENTATION.md
CODE_STYLE_ENFORCEMENT_COMPLETE.md
```

**Total: 16 files** (core + documentation)

---

## Recommended File Organization

### Option 1: Keep All at Root (Current)

**Pros:**
- Easy to find
- High visibility
- AI can discover via glob/grep

**Cons:**
- Cluttered root directory

```
Embarcadero/
├── .cursor/rules/           # Cursor auto-loads
├── docs/memory-bank/        # AI loads via 00-context-loader.mdc
├── SPEC_GOVERNANCE_GUIDE.md            # Reference
├── SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md
├── QUICK_REFERENCE_SPEC_GOVERNANCE.md
├── AI_CODE_STYLE_ANALYSIS.md
└── ...
```

### Option 2: Organize by Purpose (Recommended)

**Pros:**
- Clean root directory
- Clear separation: rules vs docs

**Cons:**
- Slightly harder to discover

```
Embarcadero/
├── .cursor/rules/           # Cursor auto-loads
├── docs/
│   ├── memory-bank/         # AI core context
│   ├── guides/              # Human documentation
│   │   ├── SPEC_GOVERNANCE_GUIDE.md
│   │   ├── AI_TOOLS_SETUP_GUIDE.md
│   │   └── QUICK_REFERENCE_SPEC_GOVERNANCE.md
│   └── summaries/           # Implementation summaries
│       ├── SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md
│       ├── AI_CODE_STYLE_ANALYSIS.md
│       ├── PRE_COMMIT_HOOK_IMPLEMENTATION.md
│       └── CODE_STYLE_ENFORCEMENT_COMPLETE.md
└── ...
```

---

## Testing Your Setup

### Test 1: Cursor Loads Rules

**Steps:**
1. Open Cursor
2. Open any `.cc` file
3. Ask: "What are the code style rules for this project?"

**Expected:**
```
The code style rules are:
1. Cache-line alignment (alignas(64)) for CXL structs
2. Writer annotations on shared fields
3. Cache flush after CXL writes
...
(References .cursor/rules/10-code-style.mdc)
```

**If it doesn't work:**
- Check `.cursor/rules/10-code-style.mdc` exists
- Check frontmatter has `alwaysApply: true`
- Restart Cursor

---

### Test 2: AI Loads Memory Bank

**Steps:**
1. Open Cursor or Claude Code
2. Ask: "What is the specification hierarchy for this project?"

**Expected:**
```
The specification hierarchy is:
1. spec_deviation.md (approved improvements)
2. paper_spec.md (reference design)
3. Engineering judgment

(References docs/memory-bank/spec_deviation.md)
```

**If it doesn't work:**
- AI may not have read 00-context-loader.mdc yet
- Explicitly say: "Read .cursor/rules/00-context-loader.mdc first"

---

### Test 3: Pre-Commit Hook Works

**Steps:**
```bash
# Make a bad change
echo "obj.~MyClass();" >> test.cc
git add test.cc
git commit -m "Test"
```

**Expected:**
```
ERROR: Manual destructor call detected
See .cursor/rules/10-code-style.mdc Rule #5
```

**If it doesn't work:**
- Check `.git/hooks/pre-commit` is executable
- Run manually: `.git/hooks/pre-commit`

---

## FAQ

### Q: Do I need all the summary documents?

**A:** No, they're for humans.

**Minimum for AI:**
- `.cursor/rules/` (3 files)
- `docs/memory-bank/` (5 files)

**Optional for humans:**
- `SPEC_GOVERNANCE_GUIDE.md` - How to use the system
- Other `*_SUMMARY.md` files - What was built

**Recommendation:** Keep them in `docs/guides/` for reference.

---

### Q: Will Cursor automatically fetch spec_deviation.md?

**A:** Not directly, but:

1. Cursor loads `.cursor/rules/00-context-loader.mdc`
2. That rule tells AI to read `spec_deviation.md`
3. AI (Claude/GPT-4) should then read the file

**It's indirect:** Cursor → Rule → AI → File

---

### Q: How does Claude Code work with rules?

**A:** Claude Code doesn't auto-load `.cursor/rules/`, but:

**Current approach (works):**
1. User asks question
2. Claude Code AI reads `.cursor/rules/00-context-loader.mdc`
3. AI sees instruction to load memory-bank
4. AI loads the files

**This works in practice** because AI agents follow instructions in files they read.

---

### Q: Should I delete the summary documents?

**A:** Recommend keeping them in `docs/`:

```bash
mkdir -p docs/guides docs/summaries
mv SPEC_GOVERNANCE_GUIDE.md docs/guides/
mv QUICK_REFERENCE_SPEC_GOVERNANCE.md docs/guides/
mv SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md docs/summaries/
mv AI_CODE_STYLE_ANALYSIS.md docs/summaries/
mv PRE_COMMIT_HOOK_IMPLEMENTATION.md docs/summaries/
mv CODE_STYLE_ENFORCEMENT_COMPLETE.md docs/summaries/
```

**Why keep them:**
- Onboarding new team members
- Understanding why decisions were made
- Reference when changing the system

---

### Q: Do other AI tools need configuration?

**A:** Depends on the tool:

| Tool | Auto-loads .cursor/rules? | Configuration Needed? |
|:-----|:-------------------------|:----------------------|
| **Cursor** | ✅ Yes | No - works out of box |
| **Claude Code** | ❌ No | Relies on AI reading 00-context-loader.mdc |
| **GitHub Copilot** | ❌ No | Doesn't use rules files |
| **Windsurf** | ❓ Unknown | Check documentation |

---

### Q: What if AI doesn't follow the rules?

**Debugging steps:**

1. **Check rules exist:**
   ```bash
   ls -la .cursor/rules/
   ```

2. **Check frontmatter is valid:**
   ```yaml
   ---
   description: DESCRIPTION HERE
   globs: *
   alwaysApply: true
   ---
   ```

3. **Explicitly reference rules:**
   - Instead of: "Add a function"
   - Say: "Add a function following .cursor/rules/10-code-style.mdc"

4. **Check file glob matching:**
   - Rule has `globs: "*.cc"`
   - You're editing `test.cc`
   - Should match

5. **Restart the tool:**
   - Cursor: Restart IDE
   - Claude Code: New session

---

## Recommendations

### For Cursor Users (Primary)

**Minimal setup:**
1. ✅ Keep `.cursor/rules/` (3 files)
2. ✅ Keep `docs/memory-bank/` (5 files)
3. ✅ Keep `.git/hooks/pre-commit`
4. ✅ Keep `scripts/verify_cache_alignment.sh`

**Total: 10 files** - Cursor will work perfectly

**Optional:**
- Move summary docs to `docs/summaries/`
- Move guide docs to `docs/guides/`

---

### For Claude Code Users (Minor)

**Setup:**
1. ✅ Same as Cursor (AI will read rules)
2. ✅ Optionally create startup script
3. ✅ Keep `QUICK_REFERENCE_SPEC_GOVERNANCE.md` at root for easy access

**Usage:**
- First message: "Read .cursor/rules/00-context-loader.mdc"
- Or: Just start working, AI will discover rules

---

### For Both Tools

**Best practice:**

```bash
# Core system (always keep)
.cursor/rules/               # Auto-loaded by Cursor, read by Claude Code
docs/memory-bank/            # Loaded via 00-context-loader.mdc
.git/hooks/pre-commit        # Enforcement
scripts/verify_cache_alignment.sh

# Documentation (organize)
docs/guides/
├── SPEC_GOVERNANCE_GUIDE.md           # How to use
├── QUICK_REFERENCE_SPEC_GOVERNANCE.md # Quick ref
└── AI_TOOLS_SETUP_GUIDE.md            # This file

docs/summaries/
├── SPEC_DEVIATION_IMPLEMENTATION_SUMMARY.md
├── AI_CODE_STYLE_ANALYSIS.md
├── PRE_COMMIT_HOOK_IMPLEMENTATION.md
└── CODE_STYLE_ENFORCEMENT_COMPLETE.md
```

---

## Action Items

### Immediate (Do Now)

- [ ] Test Cursor loads rules (see Test 1 above)
- [ ] Test AI loads memory bank (see Test 2 above)
- [ ] Test pre-commit hook (see Test 3 above)

### Optional (Clean Up)

- [ ] Organize docs into `docs/guides/` and `docs/summaries/`
- [ ] Update README.md with link to AI_TOOLS_SETUP_GUIDE.md
- [ ] Create `.claude/startup.sh` if using Claude Code heavily

### Before Refactoring

- [ ] Verify all files in place
- [ ] Run `./scripts/verify_cache_alignment.sh`
- [ ] Run `.git/hooks/pre-commit` manually
- [ ] Test both Cursor and Claude Code recognize rules

---

## Summary

### What's Absolutely Necessary?

**Core (10 files):**
- `.cursor/rules/` (3 files) - Rules
- `docs/memory-bank/` (5 files) - Context
- `.git/hooks/pre-commit` (1 file) - Enforcement
- `scripts/verify_cache_alignment.sh` (1 file) - Verification

**Documentation (6 files):**
- Guide files (how to use the system)
- Summary files (what was built)

**Total: 16 files**

### Does Cursor Auto-Load?

**YES** - Cursor auto-loads `.cursor/rules/*.mdc`
**NO** - Cursor does NOT auto-load `docs/memory-bank/`
**BUT** - The rules tell AI to load memory-bank files

### Does Claude Code Auto-Load?

**NO** - Claude Code does NOT auto-load `.cursor/rules/`
**BUT** - AI will read rules when working
**WORKS** - Current setup works because AI follows instructions

### Keep the Summary Docs?

**YES** - Move to `docs/summaries/` for reference
**WHY** - Useful for onboarding and understanding decisions

---

**Status:** ✅ Setup complete
**Last Updated:** 2026-01-24
**Next Step:** Test your setup with the tests above
