# Git history purge runbook (GATED — do not run casually)

The repository's `.git` is **~4.5 GB** because large evaluation artifacts were committed to
history — almost entirely under `data/` (individual broker logs up to **1.4 GB**, latency CSVs
~170–190 MB each), plus `data_backup/` and `datathroughput/`. The current working tree no longer
tracks these (see the reorg), and `.gitignore` blocks them going forward, but **they remain in
history** and keep every clone at multi-GB.

This runbook purges them from all history with `git filter-repo`, shrinking `.git` to tens of MB.

> ⚠️ **This rewrites history.** Every commit hash changes. It is a force-push that invalidates
> all existing clones and open branches on `jaewan/Embarcadero`. Do **not** run it without the
> steps below. This is a coordinated, one-time maintenance operation.

## Preconditions

1. **Announce a freeze** to all collaborators (branches seen: `erika-*`, `benchmark-tony`,
   `corfu_replication_dev`, `cgroup-setup`, …). Everyone pushes or stashes WIP first.
2. **Merge or close open PRs** — they will need to be re-created against rewritten history.
3. Note the existing pruned/backup branches so we don't duplicate effort:
   `main-pruned`, `backup/main-before-prune-20260329`, `backup-before-removing-large-files`.
   Decide whether this pass supersedes them (it should) and delete the stale ones afterward.
4. Ensure `git filter-repo` is installed: `pip install git-filter-repo` (or `brew install git-filter-repo`).

## Procedure

Work on a **fresh mirror**, never your working clone.

```bash
# 1. Fresh mirror of the remote
git clone --mirror https://github.com/jaewan/Embarcadero.git Embarcadero-purge.git
cd Embarcadero-purge.git

# 2. Measure before
du -sh .

# 3. Purge the data trees from ALL history + strip any oversized blob (belt & suspenders)
git filter-repo \
  --path data/ \
  --path data_backup/ \
  --path datathroughput/ \
  --invert-paths
git filter-repo --strip-blobs-bigger-than 5M

# (Optional) also purge Paper/ from history if it was ever committed and is unwanted:
#   git filter-repo --path Paper/ --invert-paths

# 4. Repack & measure after
git reflog expire --expire=now --all
git gc --prune=now --aggressive
du -sh .
```

Expect `.git` to drop from ~4.5 GB to tens of MB.

## Verify before pushing

```bash
# No blob over ~5 MB should remain:
git cat-file --batch-all-objects --batch-check='%(objecttype) %(objectsize) %(objectname)' \
  | awk '$1=="blob" && $2>5000000 {print $2, $3}' | sort -rn | head

# Sanity: build the pruned tree on the testbed
git clone Embarcadero-purge.git /tmp/emb-verify && cd /tmp/emb-verify
cmake -S . -B build && cmake --build build -j    # confirm targets still build
```

## Push & re-clone

```bash
# filter-repo removes the 'origin' remote by design; re-add and force-push everything.
cd Embarcadero-purge.git
git remote add origin https://github.com/jaewan/Embarcadero.git
git push --force --mirror origin
```

Then **every collaborator must re-clone** (old clones cannot be fast-forwarded):

```bash
mv Embarcadero Embarcadero.old      # keep briefly, then delete
git clone https://github.com/jaewan/Embarcadero.git
```

## After

- Delete now-stale backup branches (`main-pruned`, `backup/*`, `backup-before-removing-large-files`)
  once the rewrite is confirmed good.
- Ask GitHub support to run GC / clear stale refs if the remote still reports large size.
- Reaffirm the `results/` convention (`results/README.md`) so this never recurs.

## Rollback

The original remote is untouched until step "Push". Keep `Embarcadero-purge.git` and one old
full clone until the team confirms the rewritten history is healthy. If anything is wrong, simply
do not force-push; discard the mirror.
