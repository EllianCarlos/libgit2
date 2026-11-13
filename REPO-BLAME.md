# Repository blame: per-file speed

## 2026-10-04 Per-file speed

This file records how the speed of `git_blame_file` was measured against
`git blame -C100` on one file. The target repository is
`googleapis/google-cloud-java`, tokenized by cregit (one token per line).
The longer log of the repository blame work (plan, step 1 to 3, proof
notes) is outside this branch. Values that are not recorded anywhere are
marked "not recorded".

## Setup

- Repository: `googleapis/google-cloud-java`, 104,471 commits (from
  `tools/repoblame/RESULTS.md`).
- Pinned revision: `59fea8a53722fd7c01eacf848d9b75f5f0fe08ad`.
- Machine: not recorded (core count, CPU model, memory, kernel, and the
  load during the runs).
- Build type of libgit2 in the earlier work: RelWithDebInfo (`-O2 -g`). Whether
  the 12-file runs used it is not recorded. The compiler and its version are not recorded.
- Metric: CPU time (user + sys) of one process per file. Wall time is not
  used.
- The earlier work ran each command with `nice -n 19` in a systemd scope
  with `MemoryMax=2G` (`tools/repoblame/RESULTS.md`). Whether the 12-file
  runs did the same is not recorded. The bench scripts of the 12-file runs
  are not on this branch.

## Method

- The git side is `git blame -C100` on the file at the pinned revision,
  one process per file. The exact command line (file argument, other
  options such as `--line-porcelain`) is not recorded. The git version is
  not recorded.
- The libgit2 side is the harness `lg2blame -C100` (named in `tools/repoblame/RESULTS.md`), which calls
  `git_blame_file` with copy detection across files in the same commit
  and threshold 100, one process per file. The exact command line of the
  harness for the 12 files is not recorded. The harness source is not on
  this branch.
- The 12 files: the rule that selects them and the list of their paths
  are not recorded. The earlier work used a sample of 199 random files
  (seed 4242, see `tools/repoblame/RESULTS.md`). Whether the 12 come from
  that sample is not recorded.
- Variants:
  - HEAD: `32e7cdf15`, bundled zlib (the build option is not recorded).
  - changes, bundled zlib: this branch (`8202141ee`), bundled zlib.
  - changes, zlib-ng: this branch, zlib-ng in compat mode (the version is not recorded). The
    build flags that select zlib-ng (CMake options, library paths) are
    not recorded.
- Equivalence check: `tools/repoblame/check.py` compares the output of
  repoblame with `git blame --line-porcelain`. The script that checked
  the 199 files for this branch is not recorded.

## Results (12 files, CPU s, one process per file)

| variant | total CPU s | vs git |
|---|---|---|
| git blame -C100 | 462.8 | 1.00 |
| HEAD, bundled zlib | 718.9 | 1.55 |
| changes, bundled zlib | 566.4 | 1.22 |
| changes, zlib-ng | 485.8 | 1.05 |

With the changes and zlib-ng:

- A small file: 0.22 times git.
- The typical files: 1.00 to 1.05 times git.
- The large files: 1.02 to 1.09 times git.

The per-file CPU seconds, the file indexes and the number of files in each
group are not recorded. Only the totals above and these ranges are
recorded (pull request body).

Equivalence on the 199 random files: 199 of 199 files equal to
`git blame -C100`, 474,664 lines (pull request body). The variant used for
this check is not recorded. The git run of the same 199 files took 4,845.1
CPU s (`tools/repoblame/RESULTS.md`).

Full offline test suite: 3,036 pass, 0 fail, 42 skipped.

## Reading the source blobs of one commit

The commit `a7666a37c8` changes 103,234 files
(`tools/repoblame/RESULTS.md`). Reading its 103,031 source blobs (3.4 GB)
(commit message of `07ac964ca`):

| variant | time |
|---|---|
| before, zlib-ng | 11.5 s |
| after, LRU cache of 96 MB, zlib-ng | 10.5 s |
| LRU cache of 16 MB | 11.5 s |
| cache of 96 MB without LRU | 11.0 s |

The times of the bundled zlib and of `git cat-file --batch` are not
recorded.

## Changes that reach every libgit2 user

Two changes in this branch are not limited to blame. They change the
behavior of the library for all callers.

- Delta base cache of pack files (`src/libgit2/pack.c`, `pack.h`):
  - The memory limit of the cache of each pack file moves from 16 MB to
    96 MB. This is the default of git's `core.deltaBaseCacheLimit`. A
    process that opens many pack files can now hold up to 96 MB of delta
    bases per pack file.
  - The cache is now LRU. Before, a full cache freed all entries that
    nobody used. Now it frees the least recently used entries until the
    new base fits.
  - When the new base does not fit (all entries are in use), it is not
    added to the cache.
  - The limit for one cached object (1 MB) does not change.
- Tree diffs (`src/libgit2/diff_generate.c`): `git_diff_tree_to_tree`
  takes a path in which subtrees are not expanded:
  - It is used when both trees are given, `opts` has no pathspec, no
    progress callback, and none of `GIT_DIFF_INCLUDE_UNMODIFIED`,
    `GIT_DIFF_IGNORE_CASE` and `GIT_DIFF_INCLUDE_TYPECHANGE_TREES`. It
    is also used when `opts` is NULL.
  - Two subtrees at the same path with the same id are skipped. Other
    subtrees are entered.
  - The callbacks and the delta list see the same files in the same
    order as before. The result does not change. The speed does.
  - A diff with a pathspec, a progress callback, or one of the three
    flags, and a diff with a missing tree, use the old path.

The other changes (xdiff line hash, suspect order in blame, blob sharing
in `find_origin`) change the speed. The xdiff hash changes the hash
values, but xdiff compares only hashes made with the same flags, and the
final test uses `xdl_recmatch()`. The suspect order in blame changes the
order of work, not the result (199 of 199 files equal to git).

## Gap that is left

Large files: the xdiff preparation of the copy search (`xdl_prepare_ctx`,
`xdl_cha_alloc`, malloc). git removed the chastore from `xdfile_t` and
changed `xprepare.c` after 2.40.1. libgit2 has no local change in
`xprepare.c`. Next step: update `deps/xdiff` to the version of git.

## Commits (branch `perf/per-file-blame`, from `32e7cdf15`)

- `07ac964ca` pack: LRU delta base cache, limit 96 MB.
- `0379814cc` xdiff: git's line hash.
- `09cc707f5` diff: skip equal subtrees, with a test.
- `d840c380b` blame: suspect with the newest commit first.
- `8202141ee` blame: share the blob in `find_origin`.

The semantics of the proof (Lean model in `repoblame-proof`, at
`3cbd497e5`) use the old suspect order.
