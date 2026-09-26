#include "blame_helpers.h"


/**
 * The test repo has a history that looks like this:
 *
 * * (A) bc7c5ac
 * |\
 * | * (B) aa06ecc
 * * | (C) 63d671e
 * |/
 * * (D) da23739
 * * (E) b99f7ac
 *
 */

static git_repository *g_repo = NULL;

void test_blame_harder__initialize(void)
{
	cl_git_pass(git_repository_open(&g_repo, cl_fixture("blametest.git")));
}

void test_blame_harder__cleanup(void)
{
	git_repository_free(g_repo);
	g_repo = NULL;
}



static void check_b_txt(git_blame *blame, bool copies)
{
	cl_assert_equal_i(4, git_blame_hunkcount(blame));

	if (copies) {
		check_blame_hunk_index(g_repo, blame, 0, 1, 4, 1,
			"b99f7ac", "a.txt");
		check_blame_hunk_index(g_repo, blame, 1, 5, 1, 1,
			"b99f7ac", "b.txt");
		check_blame_hunk_index(g_repo, blame, 2, 6, 5, 1,
			"b99f7ac", "a.txt");
	} else {
		check_blame_hunk_index(g_repo, blame, 0, 1, 4, 0,
			"da23739", "b.txt");
		check_blame_hunk_index(g_repo, blame, 1, 5, 1, 1,
			"b99f7ac", "b.txt");
		check_blame_hunk_index(g_repo, blame, 2, 6, 5, 0,
			"63d671e", "b.txt");
	}

	check_blame_hunk_index(g_repo, blame, 3, 11, 5, 0, "aa06ecc", "b.txt");
}

void test_blame_harder__m(void)
{
	git_blame *blame;
	git_blame_options opts = GIT_BLAME_OPTIONS_INIT;

	/* No line of b.txt moved within b.txt. */
	opts.flags = GIT_BLAME_TRACK_COPIES_SAME_FILE;
	cl_git_pass(git_blame_file(&blame, g_repo, "b.txt", &opts));
	check_b_txt(blame, false);
	git_blame_free(blame);
}


void test_blame_harder__c(void)
{
	git_blame *blame;
	git_blame_options opts = GIT_BLAME_OPTIONS_INIT;

	/*
	 * Attribute the first hunk in b.txt to (E), since it was cut/pasted
	 * from a.txt in (D).  Attribute the third hunk to (E) too: (C)
	 * copied it from a.txt, which (C) also modified.
	 */
	opts.flags = GIT_BLAME_TRACK_COPIES_SAME_COMMIT_MOVES;
	cl_git_pass(git_blame_file(&blame, g_repo, "b.txt", &opts));
	check_b_txt(blame, true);
	cl_assert_equal_i(22,
		git_blame_hunk_byindex(blame, 0)->orig_start_line_number);
	cl_assert_equal_i(7,
		git_blame_hunk_byindex(blame, 2)->orig_start_line_number);
	git_blame_free(blame);
}

void test_blame_harder__cc(void)
{
	git_blame *blame;
	git_blame_options opts = GIT_BLAME_OPTIONS_INIT;

	/*
	 * The same as above: all the copies came from a.txt, which the
	 * commits that copied them also modified.
	 */
	opts.flags = GIT_BLAME_TRACK_COPIES_SAME_COMMIT_COPIES;
	cl_git_pass(git_blame_file(&blame, g_repo, "b.txt", &opts));
	check_b_txt(blame, true);
	git_blame_free(blame);
}

void test_blame_harder__ccc(void)
{
	git_blame *blame;
	git_blame_options opts = GIT_BLAME_OPTIONS_INIT;

	/*
	 * The fourth hunk in b.txt stays with (B): its lines are not in
	 * any file of (D), the parent of (B), like `git blame -C -C -C`.
	 */
	opts.flags = GIT_BLAME_TRACK_COPIES_ANY_COMMIT_COPIES;
	cl_git_pass(git_blame_file(&blame, g_repo, "b.txt", &opts));
	check_b_txt(blame, true);
	git_blame_free(blame);
}
