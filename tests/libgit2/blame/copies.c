#include "blame_helpers.h"

/*
 * The test repository "blamecopies.git" has this history, on `master`
 * unless noted:
 *
 * c1 (37495f8) create every file; root commit
 * c2 (3da1ad7) move a block of 60 alphanumeric characters to the end
 *              of move.txt
 * c3 (c8343a8) move a block from from.txt to to.txt
 * c4 (15273bd) create thresh.txt from four blocks moved out of
 *              thresh-src.txt; the blocks have 39, 40, 99 and 100
 *              alphanumeric characters
 * c5 (ddda373) create newcopy.txt with a block copied from the
 *              unmodified library.txt
 * c6 (bbdd55f) add a block copied from library.txt to existing.txt
 * c7 (5956d82) move lines of bytes from bin-src.bin to binary.bin
 * c8 (52bb6a6) add "a", "", "b" after "b" in indent.txt
 * c9 (8caa2f6) delete dup-a/other.txt (c1), dup-b/same.txt (c2) and
 *              dup-c/same.txt (c3), which have the same content, and
 *              add moved/same.txt with that content
 * c10 (e382011) rename lib-old.txt to lib-new.txt
 * c11 (1aa1ca2) move a block from lib-new.txt to target-r.txt
 * side (75a978e, branch from c11) move a block from merge-a.txt to
 *              merge-b.txt
 * main (c15aac2) delete the same block from merge-a.txt
 * merge (8b338b8) merge side into main
 * c12 (a604bc2) create sim-src.txt, low-src.txt, bn/a/name.txt and
 *              bn/other.txt
 * c13 (c3d1cd1) replace sim-src.txt by sim-dst.txt with 12 of its 20
 *              lines, low-src.txt by low-dst.txt with 9 of its 20 lines,
 *              and bn/a/name.txt and bn/other.txt by bn/b/name.txt with
 *              16 lines of each
 *
 * The expected results are those of `git blame` with the matching
 * options.
 */

static git_repository *g_repo;
static git_blame *g_blame;

void test_blame_copies__initialize(void)
{
	cl_git_pass(git_repository_open(&g_repo, cl_fixture("blamecopies.git")));
}

void test_blame_copies__cleanup(void)
{
	git_blame_free(g_blame);
	g_blame = NULL;
	git_repository_free(g_repo);
	g_repo = NULL;
}

static void blame(const char *path, unsigned int flags, uint16_t min_match)
{
	git_blame_options opts = GIT_BLAME_OPTIONS_INIT;

	opts.flags = flags;
	opts.min_match_characters = min_match;

	git_blame_free(g_blame);
	g_blame = NULL;
	cl_git_pass(git_blame_file(&g_blame, g_repo, path, &opts));
}

static void check_hunk(
	int idx,
	size_t start_line,
	size_t len,
	char boundary,
	const char *commit_id,
	const char *orig_path,
	size_t orig_start_line)
{
	check_blame_hunk_index(g_repo, g_blame, idx, start_line, len,
		boundary, commit_id, orig_path);
	cl_assert_equal_i(orig_start_line,
		git_blame_hunk_byindex(g_blame, idx)->orig_start_line_number);
}

void test_blame_copies__exact_rename_with_several_sources(void)
{
	/*
	 * Like git, follow the rename to the first deleted file with the
	 * same content and the same basename.
	 */
	blame("moved/same.txt", GIT_BLAME_NORMAL, 0);

	cl_assert_equal_i(1, git_blame_hunkcount(g_blame));
	check_hunk(0, 1, 3, 0, "3da1ad7", "dup-b/same.txt", 1);
}

void test_blame_copies__inexact_rename(void)
{
	blame("sim-dst.txt", GIT_BLAME_NORMAL, 0);

	cl_assert_equal_i(2, git_blame_hunkcount(g_blame));
	check_hunk(0, 1, 12, 0, "a604bc2", "sim-src.txt", 1);
	check_hunk(1, 13, 8, 0, "c3d1cd1", "sim-dst.txt", 13);
}

void test_blame_copies__inexact_rename_below_threshold(void)
{
	/* Less than half of the file is the same: git sees no rename. */
	blame("low-dst.txt", GIT_BLAME_NORMAL, 0);

	cl_assert_equal_i(1, git_blame_hunkcount(g_blame));
	check_hunk(0, 1, 20, 0, "c3d1cd1", "low-dst.txt", 1);
}

void test_blame_copies__inexact_rename_same_basename(void)
{
	/*
	 * Like git, prefer the only deleted file with the same basename
	 * when it is similar enough, although bn/other.txt is more similar.
	 */
	blame("bn/b/name.txt", GIT_BLAME_NORMAL, 0);

	cl_assert_equal_i(2, git_blame_hunkcount(g_blame));
	check_hunk(0, 1, 16, 0, "a604bc2", "bn/a/name.txt", 1);
	check_hunk(1, 17, 4, 0, "c3d1cd1", "bn/b/name.txt", 17);
}
