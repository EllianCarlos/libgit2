#!/usr/bin/env python3
"""Compare the output of repoblame with `git blame --line-porcelain`.

usage: check.py --repo REPO --rev REV --list LIST --out OUTDIR
                [--opts=-C100] [--jobs N]

LIST has one "<idx>\\t<path>" row per file, as for repoblame.  OUTDIR is
the -o directory of repoblame: OUTDIR/<idx>.txt has one line per final
line, "<commit> <orig_line> <orig_path>".  For each file, run
`git blame <opts> --line-porcelain REV -- path` with no user or system
config, and compare line by line: commit, original line, original path.
Print one row per file that differs, and a total.
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys

ENV = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null",
           GIT_CONFIG_NOSYSTEM="1", LC_ALL="C")


def git_lines(repo, rev, path, opts):
    p = subprocess.run(["git", "--git-dir=" + repo, "blame"] + opts +
                       ["--line-porcelain", rev, "--", path], env=ENV,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError(p.stderr.decode(errors="replace").strip())
    out, sha, orig, fname = [], None, None, None
    for line in p.stdout.split(b"\n"):
        if line.startswith(b"\t"):
            out.append(b"%s %d %s" % (sha, orig, fname))
            continue
        parts = line.split(b" ")
        if len(parts[0]) in (40, 64) and len(parts) >= 3 and \
                all(c in b"0123456789abcdef" for c in parts[0]):
            sha, orig = parts[0], int(parts[1])
        elif line.startswith(b"filename "):
            fname = line[9:]
    return out


def check(args, idx, path):
    mine = open(os.path.join(args.out, idx + ".txt"), "rb").read()
    mine = mine.splitlines()
    theirs = git_lines(args.repo, args.rev, path, args.opts.split())
    n = max(len(mine), len(theirs))
    same = sum(1 for a, b in zip(mine, theirs) if a == b)
    return idx, path, n, same


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="the .git directory")
    ap.add_argument("--rev", required=True)
    ap.add_argument("--list", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--opts", default="-C100")
    ap.add_argument("--jobs", type=int, default=1)
    args = ap.parse_args()

    rows = []
    for line in open(args.list, encoding="utf-8", errors="surrogateescape"):
        if "\t" in line:
            idx, path = line.rstrip("\n").split("\t", 1)
            rows.append((idx, path))

    total = match = bad = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for idx, path, n, same in ex.map(lambda r: check(args, *r), rows):
            total += n
            match += same
            if same != n:
                bad += 1
                print("DIFF\t%s\t%s\t%d of %d lines differ"
                      % (idx, path, n - same, n))
    print("files=%d files_that_differ=%d lines=%d match=%d rate=%.4f%%"
          % (len(rows), bad, total, match,
             100.0 * match / total if total else 100.0))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
