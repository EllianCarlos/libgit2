# repoblame (experiment)

`repoblame` blames many files of one repository in one process, with
`git_blame_file_cached()` (see `include/git2/sys/blame.h`). The files
share a cache of the copy search: for each (commit, parent) pair, the
list of the files to search for copied lines, and a line filter for each
of these files. The result for each file is the same as the result of
`git blame` with the same options.

This tool is an experiment. The API in `git2/sys/blame.h` can change.

## Build

Build libgit2 first (the tool uses its shared library):

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build

Then build the tool:

    cc -O2 -o repoblame tools/repoblame/repoblame.c -Iinclude \
       -Lbuild -lgit2 -lpthread -Wl,-rpath,$PWD/build

## Run

Make a list with one `<idx>` TAB `<path>` row per file. The index names
the output file:

    git --git-dir=REPO.git ls-tree -r --name-only REV \
      | grep '\.java$' | awk '{print NR "\t" $0}' > list.txt

Blame the files with `-C100` (like `git blame -C100`):

    mkdir out
    ./repoblame -C100 -o out -s stats.tsv REPO.git REV list.txt

- `out/<idx>.txt` has one line for each line of the file:
  `<commit> <orig_line> <orig_path>`.
- `stats.tsv` has one row per file (`idx pass thread wall_s cpu_s
  lines rc list_misses`) and the counters of the cache at the end.

Options:

| option | meaning |
|---|---|
| `-M[n]`, `-C[n]` (up to 3 times), `-w` | like `git blame` |
| `-m MiB` | memory limit of the cache (default 512; the limit is soft) |
| `-j N` | N threads, each with its own `git_repository`, one shared cache |
| `-r N` | blame the list N times (pass 2 shows the cost with a warm cache) |
| `-n` | no cache (plain `git_blame_file`, one process) |
| `-c MiB` | size of the libgit2 object cache of each repository (default 256); use `-c 64` with `-j 2` to stay below 2 GB |
| `-O MiB` | libgit2 object cache size, and keep trees and blobs up to 4 MiB (measured: slower; do not use) |

The tool turns off the hash verification of objects
(`GIT_OPT_ENABLE_STRICT_HASH_VERIFICATION`), because git does not verify
them either.

## Equivalence check against git

`check.py` runs `git blame <opts> --line-porcelain REV -- <path>` for each
file of the list (with no user or system git config), and compares each
line with `out/<idx>.txt`: commit, original line, original path.

    python3 tools/repoblame/check.py --repo REPO.git --rev REV \
        --list list.txt --out out --opts=-C100 --jobs 2

It prints a row for each file that differs, and a total. The exit code
is 1 when a file differs.

## Results

See `RESULTS.md` in this directory: measurements on
googleapis/google-cloud-java (tokenized by cregit, 131,363 files).
