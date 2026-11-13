# Repository blame: per-file speed

This file records how the speed of `git_blame_file` was measured against
`git blame -C100` on one file. The target repository is
`googleapis/google-cloud-java`, tokenized by cregit (one token per line).
The longer log of the repository blame work (plan, step 1 to 3, proof
notes) is outside this branch. Values that are not recorded anywhere are
marked "not recorded".

## Setup

- Repository: `googleapis/google-cloud-java`, bare shared clone, 104,471
  commits.
- Pinned revision: `59fea8a53722fd7c01eacf848d9b75f5f0fe08ad`.
- Machine: 16 cores. During the runs the 1-minute load was 17 to 20 (from
  other work). The CPU model, the memory size and the kernel are not
  recorded.
- Build type of libgit2: RelWithDebInfo (`-O2 -g`), the same for all
  libgit2 variants. The compiler and its version are not recorded.
- Metric: CPU time (user + sys) of one process per file. Wall time is not
  used.
- Each command ran with `nice -n 19` in a systemd scope with
  `MemoryMax=2G`, after a load gate. The runner records user and sys time
  and peak RSS from `wait4()`. The order of git and libgit2 alternates per
  file.

## Method

- The git side is `git blame -C100` on the file at the pinned revision,
  one process per file. The exact command line (file argument, other
  options such as `--line-porcelain`) is not recorded. The git version is
  not recorded.
- The libgit2 side is the harness `lg2blame -C100`, which calls
  `git_blame_file` with copy detection across files in the same commit
  and threshold 100, one process per file. The exact command line of the
  harness for the 12 files is not recorded. The harness source is not on
  this branch.
- The 12 files: the rule that selects them and the list of their paths
  are not recorded. They are a subset of the 199 random files of the
  sample used for the earlier work (seed 4242, four strata by the number
  of commits that change 10,000 or more files, see
  `tools/repoblame/RESULTS.md`). That the 12 are drawn from this sample is
  not recorded either. The sections below call files by their index
  (file 2, file 31, file 71). The index table is not recorded.
- Variants:
  - HEAD: `32e7cdf15`, bundled zlib (`USE_COMPRESSION=builtin`).
  - changes, bundled zlib: this branch (`8202141ee`), bundled zlib.
  - changes, zlib-ng: this branch, zlib-ng 2.3.3 in compat mode. The
    build flags that select zlib-ng (CMake options, library paths) are
    not recorded.
- Equivalence check: the output (commit, original path, original line) of
  each line is compared with `git blame -C100`. The comparison script is
  not on this branch.

## Results (12 files, CPU s, one process per file)

| variant | total CPU s | vs git |
|---|---|---|
| git blame -C100 | 462.8 | 1.00 |
| HEAD, bundled zlib | 718.9 | 1.55 |
| changes, bundled zlib | 566.4 | 1.22 |
| changes, zlib-ng | 485.8 | 1.05 |

With the changes and zlib-ng:

- Small file 31: 0.22 times git (0.02 s).
- Six files of 17 to 22 s: 1.00 to 1.05 times git.
- Four files of 35 to 87 s: 1.02 to 1.09 times git.
- File 2 (105 s): 1.08 times git.

The per-file CPU seconds of each variant are not recorded. Only the totals
above and the ranges in this list are recorded. A first measurement of
HEAD gave 1.56 times git; the table uses 1.55 from the final run.

All variants: 31,019 of 31,019 lines equal to `git blame -C100`.

Equivalence with the changes and zlib-ng on the 199 random files: 199 of
199 files equal to git, 474,664 lines. CPU 5,089 s (the git run of the
same files took 4,845 s, with other load).

Full offline test suite: 3,036 pass, 0 fail, 42 skipped.

## Reading the source blobs of one commit

The commit `a7666a37c8` changes 103,234 files. Reading the 103,031 source
blobs of its parent (3.4 GB):

| variant | time |
|---|---|
| bundled zlib | 18.1 s |
| system zlib 1.3.2 | 18.1 s |
| zlib-ng 2.3.3 (compat) | 11.5 s |
| `git cat-file --batch` | 10.4 s user |
| zlib-ng, LRU cache of 96 MB (this branch) | 10.5 s |
| zlib-ng, LRU cache of 16 MB | 11.5 s |
| zlib-ng, flush cache of 96 MB | 11.0 s |

A private `git_blame_cache` for one file is slower (file 71: 2.15 times
git), so it is not used.

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
