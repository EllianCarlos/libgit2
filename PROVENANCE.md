# Provenance of code ported from git

This file lists every block of code in the blame work that was ported from
git.git. Everything else in these commits was written anew from the behavior
of git, and it is not listed here.

libgit2 may take code from git only when every author consented to the
relicensing (see `git.git-authors`). The origin of each block below is
licensed under the GPL-2.0, the license of git.git (see its `COPYING` file).
The file headers of the origin files were not re-read when this file was
written.

Authors are named by the upstream commit, not by name. Look them up in
git.git with `git blame` on the origin file at the commit given.

| libgit2 file | Function | Origin file | Origin function | Origin commit | Authors |
| --- | --- | --- | --- | --- | --- |
| `src/libgit2/blame_git.c` | `count_chunks` (only the loop that cuts and hashes the chunks) | `diffcore-delta.c` | `hash_chars` (lines 140-164) | `0f8e75abebff0877cae681a3d5ff31ac47f54220` | authors as recorded in the upstream commit `0f8e75abebff0877cae681a3d5ff31ac47f54220` |
| `src/libgit2/blame_git.c` | `blame_entry_score` | `builtin/blame.c` | `ent_score` (lines 806-833) | `35936f8fc38a214cc7d2595070641d39a541dfb1` | authors as recorded in the upstream commit `35936f8fc38a214cc7d2595070641d39a541dfb1` |
| `src/libgit2/blame_git.c` | `copy_split_if_better` | `builtin/blame.c` | `copy_split_if_better` (lines 835-859) | `35936f8fc38a214cc7d2595070641d39a541dfb1` | authors as recorded in the upstream commit `35936f8fc38a214cc7d2595070641d39a541dfb1` |
| `src/libgit2/blame_git.c` | `handle_split` | `builtin/blame.c` | `handle_split` (lines 861-892) | `35936f8fc38a214cc7d2595070641d39a541dfb1` | authors as recorded in the upstream commit `35936f8fc38a214cc7d2595070641d39a541dfb1` |
| `src/libgit2/blame_git.c` | `handle_split_cb` and its data type | `builtin/blame.c` | `handle_split_cb` (lines 894-912) | `35936f8fc38a214cc7d2595070641d39a541dfb1` | authors as recorded in the upstream commit `35936f8fc38a214cc7d2595070641d39a541dfb1` |

## Followed, not ported

The commit that adds the search for lines moved or copied from other files
says that the dispatch in `pass_blame` follows `builtin/blame.c` at
`35936f8fc38a214cc7d2595070641d39a541dfb1`. The commit message records no
function or line range, and the code carries no marker. Treat it as written
anew from the behavior of git unless an author of the origin says otherwise.
The origin function and lines are not recorded.

## Not part of this work

`index_blob_lines` in `src/libgit2/blame.c` is adapted from git.git too. It
was already in libgit2 before these commits, and they do not change it.
