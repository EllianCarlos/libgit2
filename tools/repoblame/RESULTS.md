# repoblame: results on googleapis/google-cloud-java

Measured on 2026-09-26/27. Repository: googleapis/google-cloud-java,
tokenized by cregit (one token per line), 104,471 commits, revision
59fea8a53722fd7c01eacf848d9b75f5f0fe08ad. Population: the 131,363
`.java` files that cregit blames. Machine: 16 cores, under load (1-min
load 23 to 27 from other work) during all runs. Each command ran with
`nice -n 19` in a systemd scope with `MemoryMax=2G`. The main metric is
CPU time (user + sys). The build type is RelWithDebInfo (`-O2 -g`).

## Sample

200 files, seed 4242: 199 random files in 4 strata, plus Playbook.java.
The strata use the number of "huge" commits (10,000 or more changed
files; there are 14) that touch the file at its current path.

| stratum | huge touches | population | sample |
|---|---|---|---|
| s0 | 0 | 18,597 | 40 |
| s1 | 1-3 | 67,656 | 60 |
| s2 | 4-5 | 8,768 | 40 |
| s3 | 6+ | 36,342 | 59 |

## Where the time goes (libgit2 before this work)

`git_blame_file` with `-C100` (copy detection across files in the same
commit, threshold 100), hash verification off, 199 files, phase timers:

| phase | share |
|---|---|
| copy search: blob reads of the sources | 58.6% |
| copy search: xdiff against the sources | 37.2% (80% of it is xdiff line preparation) |
| everything else | 4.2% |

The copy search reads each source blob 35.5 times on average (in other
files, and again in each round of the same search). Only 0.9% of its
xdiff calls give a split above the threshold. One commit
(`a7666a37c8`, 103,234 changed files) costs 60% of all the time; 162 of
the 199 files search it for copies.

## What repoblame changes

- One process for all files, and a cache of the copy search: for each
  (commit, parent) pair, the list of sources, and for each source a
  Bloom filter of its lines and of its pairs of adjacent lines (10 bits
  per item, 3 probes, a salt per source).
- The search skips a (candidate, source) pair when the filter proves that
  the source cannot give a split above the threshold, or a split that
  scores at least as high as the best split of the candidate so far. A
  split is a run of consecutive equal lines, so its score is at most the
  best run of candidate lines whose adjacent pairs can be in the source.
- The same rule in the move search in the same file.
- Threads share the cache (each thread has its own `git_repository`).

Without the salt, one false positive line was a false positive in 41,091
similar (generated) sources at the same time.

## Results: 199 random files

| variant | processes | CPU s | wall s | CPU vs git |
|---|---|---|---|---|
| git blame -C100 | 199 | 4,845.1 | 5,197.6 | 1.00x |
| libgit2 before (lg2blame -C100 -H) | 199 | 7,428.9 | 7,871.2 | 0.65x |
| repoblame, 1 thread, cold cache | 1 | 875.2 | 907.7 | 5.54x |
| repoblame, 1 thread, warm cache (second pass) | 1 | 600.7 | 635.5 | 8.07x |
| repoblame, 2 threads, `-m 512 -c 64` | 1 | 1,035.9 | 618.1 | 4.68x |
| repoblame, 1 thread, `-m 512 -c 64` | 1 | 1,011.0 | 1,050.5 | 4.79x |

(The warm-cache row is from library commit `0874dc10b`; the other
repoblame rows are from `3cbd497e5`.)

Equivalence with git (commit, original line, original path, for each
line): all 199 files, 474,664 lines, 100.0000%, for every run above.

Peak RSS of one repoblame process: 2.1 to 2.9 GB, of which about 1 GB is
file-backed pack pages. With 2 threads, the default libgit2 object cache
(per repository) and a 1 GiB blame cache went above the 2 GB cgroup
limit; `-m 512 -c 64` stays at about 1 GB in the cgroup.

2 threads against 1 thread, same options: 1.70 times less wall time, 2.5%
more CPU time.

## Exactness check

A profile build with `GIT_BLAME_CHECK_PRUNE=1` runs the xdiff for each
skipped pair and reports a split above the bar. On all 199 files:
27,608,806 skipped pairs (copy and move search), 0 violations, and the
output is 100% equal to git. The blame tests of libgit2 (57, including
new tests that compare the cached and uncached results on the fixture
repository with 8 option sets) pass.

Not verified: the full 131,363-file run, the scaling beyond 2 threads,
and the time of the tail (one file measured).

## Projection to all 131,363 files (CPU hours)

Stratified mean with a 90% bootstrap interval (2,000 resamples),
Playbook.java not included:

| variant | CPU h | 90% interval |
|---|---|---|
| git blame -C100 | 848 | 777 - 932 |
| libgit2 before | 1,306 | 1,205 - 1,422 |
| repoblame `3cbd497e5`, 1 thread, cold cache | 129 | 98 - 166 |
| repoblame `0874dc10b`, 1 thread, cold cache | 141 | 107 - 181 |
| repoblame `0874dc10b`, 1 thread, warm cache | 91 | 68 - 118 |
| repoblame `3cbd497e5`, 2 threads, `-m 512 -c 64` | 157 | 118 - 202 |

The full run is between the cold and warm rows. The projection for git
agrees with the pipeline: 110 to 145 h of wall time at about 8 parallel
jobs.

## The slow tail: large files that a huge commit adds

Playbook.java (514 KB, 30,843 token lines) is new in a commit that
changes 47,629 files. At its creation, the copy search compares the
whole file with 47,060 sources, in many rounds.

| variant | CPU s |
|---|---|
| git blame -C100 | 16,640 (4.62 h) |
| repoblame `aeab7a14f` (filter, no bar; in the 200-file run) | 9,930 (2.76 h) |
| repoblame `3cbd497e5` (current; alone, cold cache) | 6,587 (1.83 h) |

All three give the same 30,843 lines. In the population, 31 files larger
than 200 KB are added by a huge commit. If each costs as much as
Playbook.java, they add up to about 143 CPU h for git and 57 CPU h for
repoblame. This estimate is rough: the cost depends on the size of the
file and on how similar the other files are.
