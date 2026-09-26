/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "blame_git.h"

#include "commit.h"
#include "blob.h"
#include "diff_xdiff.h"
#include "hashmap_str.h"

#ifdef GIT_BLAME_PROFILE
/*
 * Measurement only: phase timers and counters for one process.  They are
 * not thread safe.  Set GIT_BLAME_PROFILE_LOG to a file name to get one
 * line per copy search and per rename search, and one total per blame.
 */
#include <time.h>

extern double xdl_prof_prepare_s;
double xdl_prof_prepare_s;

enum {
	P_FIND_ORIGIN, P_PARENT_XDIFF, P_MOVE_XDIFF, P_RENAME_DIFF,
	P_RENAME_LOAD, P_RENAME_COUNT, P_COPY_DIFF, P_COPY_LOAD,
	P_COPY_XDIFF, P_FILTER, P_NPHASES
};
static const char *prof_names[P_NPHASES] = {
	"find_origin", "parent_xdiff", "move_xdiff", "rename_diff",
	"rename_load", "rename_count", "copy_diff", "copy_load", "copy_xdiff",
	"filter_build"
};
static double prof_t[P_NPHASES], prof_prep[P_NPHASES];
static size_t prof_copy_searches, prof_copy_deltas, prof_copy_sources,
	prof_copy_bytes, prof_copy_xdiffs, prof_copy_useful, prof_rename_inexact,
	prof_rename_srcs, prof_rename_bytes;

static double prof_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static size_t prof_prune_violations;

/* GIT_BLAME_CHECK_PRUNE=1: run xdiff for each skipped source, too. */
static int prof_check_prune(void)
{
	static int v = -1;
	if (v < 0)
		v = getenv("GIT_BLAME_CHECK_PRUNE") != NULL;
	return v;
}

static FILE *prof_log(void)
{
	static FILE *f;
	static int tried;
	const char *p;

	if (!tried) {
		tried = 1;
		if ((p = getenv("GIT_BLAME_PROFILE_LOG")) != NULL)
			f = fopen(p, "a");
	}
	return f;
}

# define PROF_VARS double prof_t0, prof_p0
# define PROF_START() (prof_t0 = prof_now(), prof_p0 = xdl_prof_prepare_s)
# define PROF_STOP(ph) (prof_t[ph] += prof_now() - prof_t0, \
	prof_prep[ph] += xdl_prof_prepare_s - prof_p0)
# define PROF_COUNT(c, n) ((c) += (n))
#else
# define PROF_VARS int prof_unused_ = 0
# define PROF_START() (void)prof_unused_
# define PROF_STOP(ph) (void)prof_unused_
# define PROF_COUNT(c, n) (void)0
#endif

/*
 * Origin is refcounted and usually we keep the blob contents to be
 * reused.
 */
static git_blame__origin *origin_incref(git_blame__origin *o)
{
	if (o)
		o->refcnt++;
	return o;
}

static void origin_decref(git_blame__origin *o)
{
	if (o && --o->refcnt <= 0) {
		if (o->previous)
			origin_decref(o->previous);
		git_blob_free(o->blob);
		git_commit_free(o->commit);
		git__free(o);
	}
}

/*
 * Create a new origin structure for a blob at a path in a commit. On
 * success, the origin takes ownership of the commit and the blob.
 */
static int new_origin(
	git_blame__origin **out,
	git_commit *commit,
	git_blob *blob,
	const char *path,
	uint16_t mode)
{
	git_blame__origin *o;
	size_t path_len = strlen(path), alloc_len;

	GIT_ERROR_CHECK_ALLOC_ADD(&alloc_len, sizeof(*o), path_len);
	GIT_ERROR_CHECK_ALLOC_ADD(&alloc_len, alloc_len, 1);
	o = git__calloc(1, alloc_len);
	GIT_ERROR_CHECK_ALLOC(o);

	o->commit = commit;
	o->blob = blob;
	o->mode = mode;
	o->refcnt = 1;
	strcpy(o->path, path);

	*out = o;

	return 0;
}

/*
 * Look up the file at `path` in the tree of `commit`.  Sets `*out` to NULL
 * when there is no file there: the path does not exist, or it is a
 * directory or a submodule.
 */
static int lookup_file(
	git_tree_entry **out,
	git_commit *commit,
	const char *path)
{
	git_tree *tree = NULL;
	int error;

	*out = NULL;

	if ((error = git_commit_tree(&tree, commit)) < 0)
		return error;

	error = git_tree_entry_bypath(out, tree, path);
	git_tree_free(tree);

	if (error == GIT_ENOTFOUND) {
		git_error_clear();
		return 0;
	} else if (error < 0) {
		return error;
	}

	if (git_tree_entry_type(*out) != GIT_OBJECT_BLOB) {
		git_tree_entry_free(*out);
		*out = NULL;
	}

	return 0;
}

/*
 * Create a new origin for the file `entry` at `path` in `commit`. On
 * success, the origin takes ownership of the commit.
 */
static int make_origin_from_entry(
	git_blame__origin **out,
	git_repository *repo,
	git_commit *commit,
	const char *path,
	const git_tree_entry *entry)
{
	git_blob *blob;
	int error;

	if ((error = git_blob_lookup(&blob, repo, git_tree_entry_id(entry))) < 0)
		return error;

	if ((error = new_origin(out, commit, blob, path,
			(uint16_t)git_tree_entry_filemode_raw(entry))) < 0)
		git_blob_free(blob);

	return error;
}

/* Given a commit and a path in it, create a new origin structure. */
static int make_origin(
	git_blame__origin **out,
	git_commit *commit,
	const char *path)
{
	git_tree_entry *entry;
	int error;

	if ((error = lookup_file(&entry, commit, path)) < 0)
		return error;

	if (!entry) {
		git_error_set(GIT_ERROR_INVALID,
			"the path '%s' is not a file in the commit", path);
		return GIT_ENOTFOUND;
	}

	error = make_origin_from_entry(out, git_commit_owner(commit),
		commit, path, entry);
	git_tree_entry_free(entry);

	return error;
}

/* Locate an existing origin or create a new one. */
int git_blame__get_origin(
		git_blame__origin **out,
		git_blame *blame,
		git_commit *commit,
		const char *path)
{
	git_blame__entry *e;

	for (e = blame->ent; e; e = e->next) {
		if (e->suspect->commit == commit && !strcmp(e->suspect->path, path)) {
			*out = origin_incref(e->suspect);
			return 0;
		}
	}
	return make_origin(out, commit, path);
}

typedef struct blame_chunk_cb_data {
	git_blame *blame;
	git_blame__origin *target;
	git_blame__origin *parent;
	long tlno;
	long plno;
}blame_chunk_cb_data;

static bool same_suspect(git_blame__origin *a, git_blame__origin *b)
{
	if (a == b)
		return true;
	if (git_oid_cmp(git_commit_id(a->commit), git_commit_id(b->commit)))
		return false;
	return 0 == strcmp(a->path, b->path);
}

/* find the line number of the last line the target is suspected for */
static bool find_last_in_target(size_t *out, git_blame *blame, git_blame__origin *target)
{
	git_blame__entry *e;
	size_t last_in_target = 0;
	bool found = false;

	*out = 0;

	for (e=blame->ent; e; e=e->next) {
		if (e->guilty || !same_suspect(e->suspect, target))
			continue;
		if (last_in_target < e->s_lno + e->num_lines) {
			found = true;
			last_in_target = e->s_lno + e->num_lines;
		}
	}

	*out = last_in_target;
	return found;
}

/*
 * It is known that lines between tlno to same came from parent, and e
 * has an overlap with that range.  it also is known that parent's
 * line plno corresponds to e's line tlno.
 *
 *                <---- e ----->
 *                   <------>         (entirely within)
 *                   <------------>   (extends past)
 *             <------------>         (starts before)
 *             <------------------>   (entirely encloses)
 *
 * Split e into potentially three parts; before this chunk, the chunk
 * to be blamed for the parent, and after that portion.
 */
static void split_overlap(git_blame__entry *split, git_blame__entry *e,
		size_t tlno, size_t plno, size_t same, git_blame__origin *parent)
{
	size_t chunk_end_lno;

	if (e->s_lno < tlno) {
		/* there is a pre-chunk part not blamed on the parent */
		split[0].suspect = origin_incref(e->suspect);
		split[0].lno = e->lno;
		split[0].s_lno = e->s_lno;
		split[0].num_lines = tlno - e->s_lno;
		split[1].lno = e->lno + tlno - e->s_lno;
		split[1].s_lno = plno;
	} else {
		split[1].lno = e->lno;
		split[1].s_lno = plno + (e->s_lno - tlno);
	}

	if (same < e->s_lno + e->num_lines) {
		/* there is a post-chunk part not blamed on parent */
		split[2].suspect = origin_incref(e->suspect);
		split[2].lno = e->lno + (same - e->s_lno);
		split[2].s_lno = e->s_lno + (same - e->s_lno);
		split[2].num_lines = e->s_lno + e->num_lines - same;
		chunk_end_lno = split[2].lno;
	} else {
		chunk_end_lno = e->lno + e->num_lines;
	}
	split[1].num_lines = chunk_end_lno - split[1].lno;

	/*
	 * if it turns out there is nothing to blame the parent for, forget about
	 * the splitting. !split[1].suspect signals this.
	 */
	if (split[1].num_lines < 1)
		return;
	split[1].suspect = origin_incref(parent);
}

/*
 * Link in a new blame entry to the scoreboard. Entries that cover the same
 * line range have been removed from the scoreboard previously.
 */
static void add_blame_entry(git_blame *blame, git_blame__entry *e)
{
	git_blame__entry *ent, *prev = NULL;

	origin_incref(e->suspect);

	for (ent = blame->ent; ent && ent->lno < e->lno; ent = ent->next)
		prev = ent;

	/* prev, if not NULL, is the last one that is below e */
	e->prev = prev;
	if (prev) {
		e->next = prev->next;
		prev->next = e;
	} else {
		e->next = blame->ent;
		blame->ent = e;
	}
	if (e->next)
		e->next->prev = e;
}

/*
 * src typically is on-stack; we want to copy the information in it to
 * a malloced blame_entry that is already on the linked list of the scoreboard.
 * The origin of dst loses a refcnt while the origin of src gains one.
 */
static void dup_entry(git_blame__entry *dst, git_blame__entry *src)
{
	git_blame__entry *p, *n;

	p = dst->prev;
	n = dst->next;
	origin_incref(src->suspect);
	origin_decref(dst->suspect);
	memcpy(dst, src, sizeof(*src));
	dst->prev = p;
	dst->next = n;
	dst->score = 0;
}

/*
 * split_overlap() divided an existing blame e into up to three parts in split.
 * Adjust the linked list of blames in the scoreboard to reflect the split.
 * If tail is not NULL, it is set to the entry that holds the part after the
 * chunk, or to NULL if there is no such part.
 */
static int split_blame(
		git_blame *blame,
		git_blame__entry *split,
		git_blame__entry *e,
		git_blame__entry **tail)
{
	git_blame__entry *new_entry;

	if (tail)
		*tail = NULL;

	if (split[0].suspect && split[2].suspect) {
		/* The first part (reuse storage for the existing entry e */
		dup_entry(e, &split[0]);

		/* The last part -- me */
		new_entry = git__malloc(sizeof(*new_entry));
		GIT_ERROR_CHECK_ALLOC(new_entry);
		memcpy(new_entry, &(split[2]), sizeof(git_blame__entry));
		add_blame_entry(blame, new_entry);
		if (tail)
			*tail = new_entry;

		/* ... and the middle part -- parent */
		new_entry = git__malloc(sizeof(*new_entry));
		GIT_ERROR_CHECK_ALLOC(new_entry);
		memcpy(new_entry, &(split[1]), sizeof(git_blame__entry));
		add_blame_entry(blame, new_entry);
	} else if (!split[0].suspect && !split[2].suspect) {
		/*
		 * The parent covers the entire area; reuse storage for e and replace it
		 * with the parent
		 */
		dup_entry(e, &split[1]);
	} else if (split[0].suspect) {
		/* me and then parent */
		dup_entry(e, &split[0]);
		new_entry = git__malloc(sizeof(*new_entry));
		GIT_ERROR_CHECK_ALLOC(new_entry);
		memcpy(new_entry, &(split[1]), sizeof(git_blame__entry));
		add_blame_entry(blame, new_entry);
	} else {
		/* parent and then me */
		dup_entry(e, &split[1]);
		new_entry = git__malloc(sizeof(*new_entry));
		GIT_ERROR_CHECK_ALLOC(new_entry);
		memcpy(new_entry, &(split[2]), sizeof(git_blame__entry));
		add_blame_entry(blame, new_entry);
		if (tail)
			*tail = new_entry;
	}

	return 0;
}

/*
 * After splitting the blame, the origins used by the on-stack blame_entry
 * should lose one refcnt each.
 */
static void decref_split(git_blame__entry *split)
{
	int i;
	for (i=0; i<3; i++)
		origin_decref(split[i].suspect);
}

/*
 * Helper for blame_chunk(). blame_entry e is known to overlap with the patch
 * hunk; split it and pass blame to the parent.
 */
static int blame_overlap(
		git_blame *blame,
		git_blame__entry *e,
		size_t tlno,
		size_t plno,
		size_t same,
		git_blame__origin *parent)
{
	git_blame__entry split[3] = {{0}};

	split_overlap(split, e, tlno, plno, same, parent);
	if (split[1].suspect)
		if (split_blame(blame, split, e, NULL) < 0)
			return -1;
	decref_split(split);

	return 0;
}

/*
 * Process one hunk from the patch between the current suspect for blame_entry
 * e and its parent. Find and split the overlap, and pass blame to the
 * overlapping part to the parent.
 */
static int blame_chunk(
		git_blame *blame,
		size_t tlno,
		size_t plno,
		size_t same,
		git_blame__origin *target,
		git_blame__origin *parent)
{
	git_blame__entry *e;

	for (e = blame->ent; e; e = e->next) {
		if (e->guilty || !same_suspect(e->suspect, target))
			continue;
		if (same <= e->s_lno)
			continue;
		if (tlno < e->s_lno + e->num_lines) {
			if (blame_overlap(blame, e, tlno, plno, same, parent) < 0)
				return -1;
		}
	}

	return 0;
}

static int my_emit(
	long start_a, long count_a,
	long start_b, long count_b,
	void *cb_data)
{
	blame_chunk_cb_data *d = (blame_chunk_cb_data *)cb_data;

	if (blame_chunk(d->blame, d->tlno, d->plno, start_b, d->target, d->parent) < 0)
		return -1;
	d->plno = start_a + count_a;
	d->tlno = start_b + count_b;

	return 0;
}

static void trim_common_tail(mmfile_t *a, mmfile_t *b, long ctx)
{
	const int blk = 1024;
	long trimmed = 0, recovered = 0;
	char *ap = a->ptr + a->size;
	char *bp = b->ptr + b->size;
	long smaller = (long)((a->size < b->size) ? a->size : b->size);

	if (ctx)
		return;

	while (blk + trimmed <= smaller && !memcmp(ap - blk, bp - blk, blk)) {
		trimmed += blk;
		ap -= blk;
		bp -= blk;
	}

	while (recovered < trimmed)
		if (ap[recovered++] == '\n')
			break;
	a->size -= trimmed - recovered;
	b->size -= trimmed - recovered;
}

static int diff_hunks(
	mmfile_t file_a,
	mmfile_t file_b,
	xdl_emit_hunk_consume_func_t hunk_func,
	void *cb_data,
	git_blame_options *options)
{
	xdemitconf_t xecfg = {0};
	xdemitcb_t ecb = {0};
	xpparam_t xpp = {0};

	/* git blame uses the indent heuristic by default. */
	xpp.flags = XDF_INDENT_HEURISTIC;

	if (options->flags & GIT_BLAME_IGNORE_WHITESPACE)
		xpp.flags |= XDF_IGNORE_WHITESPACE;

	xecfg.hunk_func = hunk_func;
	ecb.priv = cb_data;

	trim_common_tail(&file_a, &file_b, 0);

	if (file_a.size > GIT_XDIFF_MAX_SIZE ||
		file_b.size > GIT_XDIFF_MAX_SIZE) {
		git_error_set(GIT_ERROR_INVALID, "file too large to blame");
		return -1;
	}

	return xdl_diff(&file_a, &file_b, &xpp, &xecfg, &ecb);
}

static void fill_origin_blob(git_blame__origin *o, mmfile_t *file)
{
	memset(file, 0, sizeof(*file));
	if (o->blob) {
		file->ptr = (char*)git_blob_rawcontent(o->blob);
		file->size = (long)git_blob_rawsize(o->blob);
	}
}

static int pass_blame_to_parent(
		git_blame *blame,
		git_blame__origin *target,
		git_blame__origin *parent)
{
	size_t last_in_target;
	mmfile_t file_p, file_o;
	blame_chunk_cb_data d = { blame, target, parent, 0, 0 };

	if (!find_last_in_target(&last_in_target, blame, target))
		return 1; /* nothing remains for this target */

	fill_origin_blob(parent, &file_p);
	fill_origin_blob(target, &file_o);

	{
		int r;
		PROF_VARS;
		PROF_START();
		r = diff_hunks(file_p, file_o, my_emit, &d, &blame->options);
		PROF_STOP(P_PARENT_XDIFF);
		if (r < 0)
			return -1;
	}

	/* The reset (i.e. anything after tlno) are the same as the parent */
	if (blame_chunk(blame, d.tlno, d.plno, last_in_target, target, parent) < 0)
		return -1;

	return 0;
}

/*
 * Look for the file of `origin` at the same path in `parent`.  Sets `*out`
 * to NULL when the path is not a file of the same type there, like git
 * does for a path that the commit added or changed the type of.
 */
static int find_origin(
		git_blame__origin **out,
		git_blame *blame,
		git_commit *parent,
		git_blame__origin *origin)
{
	git_tree_entry *entry;
	int error;

	*out = NULL;

	if ((error = lookup_file(&entry, parent, origin->path)) < 0 || !entry)
		return error;

	if (GIT_MODE_TYPE(git_tree_entry_filemode_raw(entry)) ==
	    GIT_MODE_TYPE(origin->mode))
		error = make_origin_from_entry(out, blame->repository, parent,
			origin->path, entry);

	git_tree_entry_free(entry);
	return error;
}

/* Whether the last path components of `a` and `b` are the same. */
static bool same_basename(const char *a, const char *b)
{
	const char *a_base = strrchr(a, '/'), *b_base = strrchr(b, '/');

	return strcmp(a_base ? a_base + 1 : a, b_base ? b_base + 1 : b) == 0;
}

/*
 * The number of identical files that git considers, at most, when it
 * picks the source of an exact rename.
 */
#define BLAME_EXACT_RENAME_CANDIDATES 100

/*
 * Pick the file that `dst` is an exact rename of, among the files that
 * the diff deletes, like git: the first one in path order with the same
 * contents and the same basename, or else the first one with the same
 * contents.  A file that is not a regular file must have the same mode.
 */
static const git_diff_delta *find_exact_rename(
		git_diff *diff,
		const git_diff_delta *dst)
{
	const git_diff_delta *best = NULL;
	size_t i, candidates = 0;

	for (i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);

		if (delta->status != GIT_DELTA_DELETED ||
		    !git_oid_equal(&delta->old_file.id, &dst->new_file.id))
			continue;

		if ((!GIT_MODE_ISBLOB(delta->old_file.mode) ||
		     !GIT_MODE_ISBLOB(dst->new_file.mode)) &&
		    delta->old_file.mode != dst->new_file.mode)
			continue;

		if (!best)
			best = delta;

		if (same_basename(delta->old_file.path, dst->new_file.path)) {
			best = delta;
			break;
		}

		if (++candidates == BLAME_EXACT_RENAME_CANDIDATES)
			break;
	}

	return best;
}

/*
 * The scores of git's rename detection: a similarity of 100% is 60000,
 * a rename needs 50%, and a rename to a file with the same basename, when
 * there is only one candidate with that basename, needs 75%.
 */
#define BLAME_RENAME_MAX_SCORE 60000.0
#define BLAME_RENAME_SCORE 30000
#define BLAME_RENAME_BASENAME_SCORE 45000

/* How many candidates git keeps for a destination of an inexact rename. */
#define BLAME_RENAME_SLOTS 4

/* git counts a file as binary when a NUL is in its first bytes. */
#define BLAME_BINARY_CHECK_BYTES 8000

/* The modulus of git's chunk hash. */
#define BLAME_HASHBASE 107927

/* How many bytes of a blob fall in chunks with a given hash. */
typedef struct {
	unsigned int hash;
	size_t bytes;
} blame_chunk_count;

typedef git_array_t(blame_chunk_count) blame_chunk_counts;

/* A file that a rename could come from or go to. */
typedef struct {
	const git_diff_delta *delta;
	git_blob *blob;
	uint16_t mode;
	bool counted;
	blame_chunk_counts counts;
} blame_rename_file;

typedef git_array_t(blame_rename_file) blame_rename_files;

static int add_chunk_count(
		blame_chunk_counts *counts,
		unsigned int hash,
		size_t bytes)
{
	blame_chunk_count *c = git_array_alloc(*counts);

	GIT_ERROR_CHECK_ALLOC(c);
	c->hash = hash;
	c->bytes = bytes;
	return 0;
}

static int chunk_count_cmp(const void *a, const void *b, void *payload)
{
	const blame_chunk_count *x = a, *y = b;

	GIT_UNUSED(payload);

	return (x->hash > y->hash) - (x->hash < y->hash);
}

/*
 * Cut the contents of `file` into chunks that end at a newline or after
 * 64 bytes, whichever comes first, and count how many bytes fall in
 * chunks of each hash, sorted by hash.  In a text file, a CR before a LF
 * is not part of any chunk.
 *
 * The loop that cuts and hashes the chunks is ported from git.git's
 * `hash_chars` in diffcore-delta.c at
 * https://github.com/git/git/blob/0f8e75abebff0877cae681a3d5ff31ac47f54220/diffcore-delta.c#L140-L164
 */
static int count_chunks(blame_rename_file *file)
{
	const unsigned char *buf = git_blob_rawcontent(file->blob);
	size_t sz = (size_t)git_blob_rawsize(file->blob);
	unsigned int accum1 = 0, accum2 = 0;
	blame_chunk_count *c, *last = NULL;
	bool is_text;
	size_t i, n = 0;
	int error;

	is_text = !memchr(buf, 0, min(sz, BLAME_BINARY_CHECK_BYTES));

	while (sz) {
		unsigned int ch = *buf++;
		unsigned int old_1 = accum1;
		sz--;

		/* Ignore CR in CRLF sequence if text */
		if (is_text && ch == '\r' && sz && *buf == '\n')
			continue;

		accum1 = (accum1 << 7) ^ (accum2 >> 25);
		accum2 = (accum2 << 7) ^ (old_1 >> 25);
		accum1 += ch;
		if (++n < 64 && ch != '\n')
			continue;
		if ((error = add_chunk_count(&file->counts,
				(accum1 + accum2 * 0x61) % BLAME_HASHBASE,
				n)) < 0)
			return error;
		n = 0;
		accum1 = accum2 = 0;
	}
	if (n > 0 &&
	    (error = add_chunk_count(&file->counts,
			(accum1 + accum2 * 0x61) % BLAME_HASHBASE, n)) < 0)
		return error;

	/* Merge the counts of the chunks that have the same hash. */
	git__qsort_r(file->counts.ptr, file->counts.size,
		sizeof(blame_chunk_count), chunk_count_cmp, NULL);

	git_array_foreach(file->counts, i, c) {
		if (last && last->hash == c->hash)
			last->bytes += c->bytes;
		else if (!last)
			last = c;
		else
			*++last = *c;
	}
	file->counts.size = last ? (size_t)(last - file->counts.ptr) + 1 : 0;

	file->counted = true;
	return 0;
}

/*
 * How similar the contents of `src` and `dst` are, like git's estimate
 * for renames: the bytes of `src` that `dst` has too, chunk by chunk,
 * against the size of the larger file, from 0 to BLAME_RENAME_MAX_SCORE.
 * Only regular files can be similar, and only when their sizes are close
 * enough to reach `minimum`.
 */
static int rename_similarity(
		int *out,
		blame_rename_file *src,
		blame_rename_file *dst,
		int minimum)
{
	size_t src_size, dst_size, max_size, delta_size;
	size_t copied = 0, i = 0, j = 0;
	int error;

	*out = 0;

	if (!GIT_MODE_ISBLOB(src->mode) || !GIT_MODE_ISBLOB(dst->mode))
		return 0;

	src_size = (size_t)git_blob_rawsize(src->blob);
	dst_size = (size_t)git_blob_rawsize(dst->blob);
	max_size = max(src_size, dst_size);
	delta_size = max_size - min(src_size, dst_size);

	if ((double)max_size * (BLAME_RENAME_MAX_SCORE - minimum) <
	    (double)delta_size * BLAME_RENAME_MAX_SCORE || !dst_size)
		return 0;

	{
		PROF_VARS;
		PROF_START();
		error = 0;
		if ((!src->counted && (error = count_chunks(src)) < 0) ||
		    (!dst->counted && (error = count_chunks(dst)) < 0)) {
			PROF_STOP(P_RENAME_COUNT);
			return error;
		}
		PROF_STOP(P_RENAME_COUNT);
	}

	while (i < src->counts.size && j < dst->counts.size) {
		blame_chunk_count *s = &src->counts.ptr[i],
		                  *d = &dst->counts.ptr[j];

		if (s->hash < d->hash) {
			i++;
		} else if (s->hash > d->hash) {
			j++;
		} else {
			copied += min(s->bytes, d->bytes);
			i++;
			j++;
		}
	}

	*out = (int)((double)copied * BLAME_RENAME_MAX_SCORE / (double)max_size);
	return 0;
}

/* One candidate source of an inexact rename. */
typedef struct {
	blame_rename_file *src;
	int score;
	bool same_basename;
} blame_rename_slot;

/* Whether `a` is a worse candidate than `b`; a slot without source is worst. */
static bool rename_slot_worse(
		const blame_rename_slot *a,
		const blame_rename_slot *b)
{
	if (!a->src)
		return b->src != NULL;
	if (!b->src)
		return false;
	if (a->score != b->score)
		return a->score < b->score;
	return !a->same_basename && b->same_basename;
}

static int load_rename_file(
		blame_rename_file *file,
		git_repository *repo,
		const git_diff_delta *delta,
		const git_diff_file *side)
{
	memset(file, 0, sizeof(*file));
	file->delta = delta;
	file->mode = side->mode;
	return git_blob_lookup(&file->blob, repo, &side->id);
}

static void free_rename_files(blame_rename_files *files)
{
	blame_rename_file *file;
	size_t i;

	git_array_foreach(*files, i, file) {
		git_blob_free(file->blob);
		git_array_clear(file->counts);
	}
	git_array_clear(*files);
}

/*
 * Pick the file that `dst` is an inexact rename of, among the files that
 * the diff deletes, like git does when it follows one file:
 *
 * - If exactly one deleted file has the same basename as `dst`, and it
 *   is similar enough for BLAME_RENAME_BASENAME_SCORE, take it.
 * - Else keep the BLAME_RENAME_SLOTS best candidates, in the order of
 *   the deleted files, where a candidate takes the place of the first
 *   worst one when it is better, and take the best of them, if it is
 *   similar enough for BLAME_RENAME_SCORE.  The first slot wins a tie.
 */
static int find_inexact_rename(
		const git_diff_delta **out,
		git_blame *blame,
		git_diff *diff,
		const git_diff_delta *dst)
{
	blame_rename_files srcs = GIT_ARRAY_INIT;
	blame_rename_slot slots[BLAME_RENAME_SLOTS], candidate;
	blame_rename_file target, *src, *same_base = NULL;
	size_t i, j, same_base_count = 0, best, worst;
	int score, error;
	PROF_VARS;

	*out = NULL;
	memset(slots, 0, sizeof(slots));
	PROF_START();

	if ((error = load_rename_file(&target, blame->repository, dst,
			&dst->new_file)) < 0)
		return error;

	for (i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);

		if (delta->status != GIT_DELTA_DELETED)
			continue;

		if ((src = git_array_alloc(srcs)) == NULL) {
			git_error_set_oom();
			error = -1;
			goto done;
		}

		if ((error = load_rename_file(src, blame->repository, delta,
				&delta->old_file)) < 0) {
			srcs.size--;
			goto done;
		}
		PROF_COUNT(prof_rename_bytes, git_blob_rawsize(src->blob));
	}
	PROF_STOP(P_RENAME_LOAD);
	PROF_COUNT(prof_rename_srcs, git_array_size(srcs));
	PROF_COUNT(prof_rename_inexact, 1);
#ifdef GIT_BLAME_PROFILE
	if (prof_log()) {
		char a[GIT_OID_MAX_HEXSIZE + 1];
		git_oid_tostr(a, sizeof(a), &dst->new_file.id);
		fprintf(prof_log(), "R %s %zu %.6f\n", a,
			git_array_size(srcs), prof_now() - prof_t0);
	}
#endif

	git_array_foreach(srcs, i, src) {
		if (same_basename(src->delta->old_file.path,
				dst->new_file.path)) {
			same_base = src;
			same_base_count++;
		}
	}

	if (same_base_count == 1) {
		if ((error = rename_similarity(&score, same_base, &target,
				BLAME_RENAME_BASENAME_SCORE)) < 0)
			goto done;

		if (score >= BLAME_RENAME_BASENAME_SCORE) {
			*out = same_base->delta;
			goto done;
		}
	}

	git_array_foreach(srcs, i, src) {
		if ((error = rename_similarity(&score, src, &target,
				BLAME_RENAME_SCORE)) < 0)
			goto done;

		candidate.src = src;
		candidate.score = score;
		candidate.same_basename = same_basename(
			src->delta->old_file.path, dst->new_file.path);

		for (worst = 0, j = 1; j < BLAME_RENAME_SLOTS; j++)
			if (rename_slot_worse(&slots[j], &slots[worst]))
				worst = j;

		if (rename_slot_worse(&slots[worst], &candidate))
			slots[worst] = candidate;
	}

	for (best = 0, i = 1; i < BLAME_RENAME_SLOTS; i++)
		if (rename_slot_worse(&slots[best], &slots[i]))
			best = i;

	if (slots[best].src && slots[best].score >= BLAME_RENAME_SCORE)
		*out = slots[best].src->delta;

done:
	free_rename_files(&srcs);
	git_blob_free(target.blob);
	git_array_clear(target.counts);
	return error;
}

/*
 * Look for the file that the commit of `origin` renamed to the path of
 * `origin`, in `parent`.  Sets `*out` to NULL when there is none.
 */
static int find_rename(
		git_blame__origin **out,
		git_blame *blame,
		git_commit *parent,
		git_blame__origin *origin)
{
	git_diff_options diffopts = GIT_DIFF_OPTIONS_INIT;
	git_tree *otree = NULL, *ptree = NULL;
	const git_diff_delta *dst = NULL, *src = NULL;
	git_diff *diff = NULL;
	git_blob *blob = NULL;
	size_t i;
	int error;
	PROF_VARS;

	*out = NULL;

	diffopts.context_lines = 0;
	diffopts.flags = GIT_DIFF_SKIP_BINARY_CHECK |
	                 GIT_DIFF_INCLUDE_TYPECHANGE;

	PROF_START();
	if ((error = git_commit_tree(&otree, origin->commit)) < 0 ||
	    (error = git_commit_tree(&ptree, parent)) < 0 ||
	    (error = git_diff_tree_to_tree(&diff, blame->repository,
			ptree, otree, &diffopts)) < 0)
		goto done;

	/* Only a file that the commit added can be the result of a rename. */
	for (i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);

		if (!strcmp(delta->new_file.path, origin->path)) {
			if (delta->status == GIT_DELTA_ADDED)
				dst = delta;
			break;
		}
	}

	PROF_STOP(P_RENAME_DIFF);

	if (!dst)
		goto done;

	if ((src = find_exact_rename(diff, dst)) == NULL &&
	    (error = find_inexact_rename(&src, blame, diff, dst)) < 0)
		goto done;

	if (!src)
		goto done;

	if ((error = git_blob_lookup(&blob, blame->repository,
			&src->old_file.id)) < 0 ||
	    (error = new_origin(out, parent, blob,
			src->old_file.path, src->old_file.mode)) < 0)
		git_blob_free(blob);

done:
	git_diff_free(diff);
	git_tree_free(otree);
	git_tree_free(ptree);
	return error;
}

/*
 * The blobs of origin and porigin exactly match, so everything origin is
 * suspected for can be blamed on the parent.
 */
static int pass_whole_blame(git_blame *blame,
		git_blame__origin *origin, git_blame__origin *porigin)
{
	git_blame__entry *e;

	if (!porigin->blob &&
	    git_object_lookup((git_object**)&porigin->blob, blame->repository,
				git_blob_id(origin->blob), GIT_OBJECT_BLOB) < 0)
		return -1;
	for (e=blame->ent; e; e=e->next) {
		if (!same_suspect(e->suspect, origin))
			continue;
		origin_incref(porigin);
		origin_decref(e->suspect);
		e->suspect = porigin;
	}

	return 0;
}

/*
 * The default thresholds for the detection of moved and copied lines,
 * in alphanumeric characters.  These are the defaults of `git blame -M`
 * and `git blame -C`.
 */
#define BLAME_DEFAULT_MOVE_SCORE 20
#define BLAME_DEFAULT_COPY_SCORE 40

/*
 * The threshold for lines that moved within a file.  `min_match_characters`
 * sets it only when no copy detection across files is requested; like
 * `git blame -M -C<num>`, moves in the same file then use the default.
 */
static unsigned blame_move_score(git_blame *blame)
{
	if (blame->options.min_match_characters &&
	    !(blame->options.flags & GIT_BLAME_TRACK_COPIES_SAME_COMMIT_MOVES))
		return blame->options.min_match_characters;

	return BLAME_DEFAULT_MOVE_SCORE;
}

/* The threshold for lines that moved or were copied from other files. */
static unsigned blame_copy_score(git_blame *blame)
{
	if (blame->options.min_match_characters)
		return blame->options.min_match_characters;

	return BLAME_DEFAULT_COPY_SCORE;
}

GIT_INLINE(const char *) blame_nth_line(git_blame *blame, size_t lno)
{
	size_t *offset = git_array_get(blame->line_index, lno);

	GIT_ASSERT_WITH_RETVAL(offset, NULL);
	return blame->final_buf + *offset;
}

/*
 * The lines in blame_entry after splitting blames many times can become
 * very small and trivial, and at some point it becomes pointless to
 * blame the parents.  E.g. "\t\t}\n\t}\n\n" appears everywhere in any
 * ordinary C program, and it is not worth to say it was copied from
 * totally unrelated file in the parent.
 *
 * Compute how trivial the lines in the blame_entry are.  Only ASCII
 * letters and digits count, whatever the locale.
 *
 * Ported from git.git's `ent_score` in builtin/blame.c at
 * https://github.com/git/git/blob/35936f8fc38a214cc7d2595070641d39a541dfb1/builtin/blame.c#L806-L833
 */
static unsigned blame_entry_score(git_blame *blame, git_blame__entry *e)
{
	unsigned score;
	const char *cp, *ep;

	if (e->score)
		return e->score;

	score = 1;
	cp = blame_nth_line(blame, e->lno);
	ep = blame_nth_line(blame, e->lno + e->num_lines);
	while (cp && ep && cp < ep) {
		unsigned char ch = *((const unsigned char *)cp);
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		    (ch >= '0' && ch <= '9'))
			score++;
		cp++;
	}
	e->score = score;
	return score;
}

/*
 * best_so_far[] and potential[] are both a split of an existing blame_entry
 * that passes blame to the parent.  Maintain best_so_far the best split so
 * far, by comparing potential and best_so_far and copying potential into
 * best_so_far as needed.  A later split with the same score wins.
 *
 * Ported from git.git's `copy_split_if_better` in builtin/blame.c at
 * https://github.com/git/git/blob/35936f8fc38a214cc7d2595070641d39a541dfb1/builtin/blame.c#L835-L859
 */
static void copy_split_if_better(
		git_blame *blame,
		git_blame__entry *best_so_far,
		git_blame__entry *potential)
{
	int i;

	if (!potential[1].suspect)
		return;
	if (best_so_far[1].suspect) {
		if (blame_entry_score(blame, &potential[1]) <
		    blame_entry_score(blame, &best_so_far[1]))
			return;
	}

	for (i = 0; i < 3; i++)
		origin_incref(potential[i].suspect);
	decref_split(best_so_far);
	memcpy(best_so_far, potential, sizeof(git_blame__entry[3]));
}

/*
 * We are looking at a part of the final image represented by
 * ent (tlno and same are offset by ent->s_lno).
 * tlno is where we are looking at in the final image.
 * up to (but not including) same match preimage.
 * plno is where we are looking at in the preimage.
 *
 * <-------------- final image ---------------------->
 *       <------ent------>
 *         ^tlno ^same
 *    <---------preimage----->
 *         ^plno
 *
 * All line numbers are 0-based.
 *
 * Ported from git.git's `handle_split` in builtin/blame.c at
 * https://github.com/git/git/blob/35936f8fc38a214cc7d2595070641d39a541dfb1/builtin/blame.c#L861-L892
 */
static void handle_split(
		git_blame *blame,
		git_blame__entry *ent,
		size_t tlno,
		size_t plno,
		size_t same,
		git_blame__origin *parent,
		git_blame__entry *split)
{
	if (ent->num_lines <= tlno)
		return;
	if (tlno < same) {
		git_blame__entry potential[3] = {{0}};
		tlno += ent->s_lno;
		same += ent->s_lno;
		split_overlap(potential, ent, tlno, plno, same, parent);
		copy_split_if_better(blame, split, potential);
		decref_split(potential);
	}
}

/*
 * Ported from git.git's `handle_split_cb` in builtin/blame.c at
 * https://github.com/git/git/blob/35936f8fc38a214cc7d2595070641d39a541dfb1/builtin/blame.c#L894-L912
 */
typedef struct handle_split_cb_data {
	git_blame *blame;
	git_blame__entry *ent;
	git_blame__origin *parent;
	git_blame__entry *split;
	long plno;
	long tlno;
} handle_split_cb_data;

static int handle_split_cb(
	long start_a, long count_a,
	long start_b, long count_b,
	void *cb_data)
{
	handle_split_cb_data *d = (handle_split_cb_data *)cb_data;

	handle_split(d->blame, d->ent, d->tlno, d->plno, start_b, d->parent,
		     d->split);
	d->plno = start_a + count_a;
	d->tlno = start_b + count_b;

	return 0;
}

/*
 * Compare the lines of the final image that `ent` covers with the whole
 * blob of `parent`, in `file_p`.  Every run of lines that the two have in
 * common is a way to pass blame for some of the lines of `ent` to
 * `parent`; store the split for the best one in `split`, which must be
 * zeroed by the caller.  `split[1].suspect` stays NULL if the two have no
 * line in common.
 */
static int find_copy_in_blob(
		git_blame *blame,
		git_blame__entry *ent,
		git_blame__origin *parent,
		git_blame__entry *split,
		mmfile_t *file_p)
{
	handle_split_cb_data data = { 0 };
	const char *first, *end;
	mmfile_t file_o;

	if ((first = blame_nth_line(blame, ent->lno)) == NULL ||
	    (end = blame_nth_line(blame, ent->lno + ent->num_lines)) == NULL)
		return -1;

	file_o.ptr = (char *)first;
	file_o.size = (long)(end - first);

	data.blame = blame;
	data.ent = ent;
	data.parent = parent;
	data.split = split;

	if (diff_hunks(*file_p, file_o, handle_split_cb, &data,
			&blame->options) < 0)
		return -1;

	/* The lines after the last difference are common, too. */
	handle_split(blame, ent, data.tlno, data.plno, ent->num_lines,
		parent, split);

	return 0;
}

/* The line filters of the copy search; see git_blame_cache below. */

#define BLAME_CACHE_DEFAULT_BYTES ((size_t)512 * 1024 * 1024)
#define BLAME_FILTER_BITS_PER_ITEM 10
#define BLAME_FILTER_MIN_BITS 64

/*
 * The filter has a single line only when the line scores above this;
 * so the filter can prove a bound only for a threshold of this or more.
 * Most lines of a tokenized file score less, and a filter without them
 * is smaller.
 */
#define BLAME_FILTER_LINE_SCORE 16

typedef struct {
	git_oid id;
	uint16_t mode;
	bool filtered;   /* `bits` is valid */
	bool no_filter;  /* never make a filter for this source */
	uint32_t nbits;  /* the number of bits in `bits` */
	uint64_t salt;   /* makes the bits of each filter different */
	unsigned char *bits;
	char *path;
} blame_source;

typedef struct {
	char *key;
	blame_source *sources;
	size_t count;
	size_t filters;
	size_t bytes;
	uint64_t last_use;
	int refcount;   /* blames that use the list now */
	bool ready;     /* made; before that, other blames wait */
} blame_source_list;

GIT_HASHMAP_STR_SETUP(blame_source_listmap, blame_source_list *);

/*
 * The lock protects the map, the counters, and the refcount and state
 * of each list.  A list that is ready does not change until it is freed,
 * so a blame reads it without the lock.
 */
struct git_blame_cache {
	git_repository *repo;
	git_mutex lock;
	git_cond ready;
	blame_source_listmap lists;
	uint64_t clock;
	git_blame_cache_stats stats;
};

GIT_INLINE(uint64_t) blame_mix(uint64_t x)
{
	x ^= x >> 33;
	x *= UINT64_C(0xff51afd7ed558ccd);
	x ^= x >> 33;
	x *= UINT64_C(0xc4ceb9fe1a85ec53);
	x ^= x >> 33;
	return x;
}

/* The hash of a line, without its newline. */
static uint64_t blame_line_hash(const char *p, size_t len)
{
	uint64_t h = UINT64_C(0xcbf29ce484222325);

	while (len--) {
		h ^= (unsigned char)*p++;
		h *= UINT64_C(0x100000001b3);
	}
	return blame_mix(h);
}

GIT_INLINE(uint64_t) blame_item_line(uint64_t h)
{
	return blame_mix(h ^ UINT64_C(0x9e3779b97f4a7c15));
}

GIT_INLINE(uint64_t) blame_item_pair(uint64_t a, uint64_t b)
{
	return blame_mix(a * UINT64_C(0x9e3779b97f4a7c15) + b);
}

/* Map 32 random bits to a bit of the filter, without a division. */
GIT_INLINE(uint32_t) blame_filter_bit(const blame_source *s, uint32_t x)
{
	return (uint32_t)(((uint64_t)x * s->nbits) >> 32);
}

/* Three probes for each item, by double hashing. */
#define BLAME_FILTER_PROBES 3

/*
 * Most sources of a big commit have most of their lines in common (they
 * are generated).  Without a salt, a line that is a false positive in
 * one filter would be a false positive in all of them, because the same
 * common lines set the same bits.  A salt for each filter makes the
 * false positives of different filters independent.
 */
GIT_INLINE(void) blame_filter_set(blame_source *s, uint64_t item)
{
	uint64_t x = blame_mix(item ^ s->salt);
	uint32_t h1 = (uint32_t)x, h2 = (uint32_t)(x >> 32) | 1;
	int i;

	for (i = 0; i < BLAME_FILTER_PROBES; i++, h1 += h2) {
		uint32_t b = blame_filter_bit(s, h1);
		s->bits[b >> 3] |= (unsigned char)(1 << (b & 7));
	}
}

GIT_INLINE(bool) blame_filter_test(const blame_source *s, uint64_t item)
{
	uint64_t x = blame_mix(item ^ s->salt);
	uint32_t h1 = (uint32_t)x, h2 = (uint32_t)(x >> 32) | 1;
	int i;

	for (i = 0; i < BLAME_FILTER_PROBES; i++, h1 += h2) {
		uint32_t b = blame_filter_bit(s, h1);
		if (!(s->bits[b >> 3] & (1 << (b & 7))))
			return false;
	}
	return true;
}

static int blame_u64_cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

/*
 * Split `buf` into lines like xdiff does (a last line without a newline
 * is a line, too), and put the hash of each line, without its newline,
 * in `*out`.
 */
static int blame_hash_lines(
		uint64_t **out,
		unsigned **scores_out,
		size_t *count,
		const char *buf,
		size_t size)
{
	unsigned *sc = NULL;
	const char *p = buf, *end = buf + size, *nl;
	size_t n = 0, alloc = 0;
	uint64_t *h = NULL;

	*out = NULL;
	*count = 0;

	for (nl = p; nl < end; nl++)
		if (*nl == '\n')
			alloc++;
	alloc++;

	h = git__mallocarray(alloc, sizeof(uint64_t));
	sc = git__mallocarray(alloc, sizeof(unsigned));
	if (!h || !sc) {
		git__free(h);
		git__free(sc);
		git_error_set_oom();
		return -1;
	}

	while (p < end) {
		const char *q;
		unsigned score = 0;

		nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl)
			nl = end;
		for (q = p; q < nl; q++) {
			unsigned char ch = *((const unsigned char *)q);
			if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			    (ch >= '0' && ch <= '9'))
				score++;
		}
		sc[n] = score;
		h[n++] = blame_line_hash(p, (size_t)(nl - p));
		p = nl + 1;
	}

	*out = h;
	*scores_out = sc;
	*count = n;
	return 0;
}

/* Make the filter of the lines of `s` from its contents. */
static int blame_filter_build(
		blame_source *s,
		const char *buf,
		size_t size)
{
	uint64_t *h = NULL, *items = NULL;
	unsigned *sc = NULL;
	size_t n, nitems = 0, distinct, i, nbits;
	int error;

	if ((error = blame_hash_lines(&h, &sc, &n, buf, size)) < 0)
		return error;

	items = git__mallocarray(n * 2 + 1, sizeof(uint64_t));
	if (!items) {
		git__free(h);
		git__free(sc);
		git_error_set_oom();
		return -1;
	}

	for (i = 0; i < n; i++) {
		if (1 + sc[i] > BLAME_FILTER_LINE_SCORE)
			items[nitems++] = blame_item_line(h[i]);
		if (i > 0)
			items[nitems++] = blame_item_pair(h[i - 1], h[i]);
	}

	qsort(items, nitems, sizeof(uint64_t), blame_u64_cmp);
	for (i = 0, distinct = 0; i < nitems; i++)
		if (!i || items[i] != items[i - 1])
			items[distinct++] = items[i];

	nbits = distinct * BLAME_FILTER_BITS_PER_ITEM;
	if (nbits < BLAME_FILTER_MIN_BITS)
		nbits = BLAME_FILTER_MIN_BITS;
	if (nbits > ((size_t)1 << 31))
		nbits = (size_t)1 << 31;
	nbits = (nbits + 7) & ~(size_t)7;

	s->bits = git__calloc(nbits / 8, 1);
	if (!s->bits) {
		git__free(h);
		git__free(sc);
		git__free(items);
		return -1;
	}
	s->nbits = (uint32_t)nbits;

	for (i = 0; i < distinct; i++)
		blame_filter_set(s, items[i]);

	s->filtered = true;
	git__free(h);
	git__free(sc);
	git__free(items);
	return 0;
}

/* The lines of a candidate entry: their hashes and their scores. */
typedef struct {
	uint64_t *hash;
	unsigned *score;
	size_t count;
} blame_entry_lines;

static int blame_entry_lines_init(
		blame_entry_lines *out,
		git_blame *blame,
		git_blame__entry *e)
{
	size_t i;

	memset(out, 0, sizeof(*out));
	out->hash = git__mallocarray(e->num_lines ? e->num_lines : 1,
		sizeof(uint64_t));
	out->score = git__mallocarray(e->num_lines ? e->num_lines : 1,
		sizeof(unsigned));
	GIT_ERROR_CHECK_ALLOC(out->hash);
	GIT_ERROR_CHECK_ALLOC(out->score);

	for (i = 0; i < e->num_lines; i++) {
		const char *cp = blame_nth_line(blame, e->lno + i);
		const char *ep = blame_nth_line(blame, e->lno + i + 1);
		size_t len;
		unsigned score = 0;

		if (!cp || !ep)
			return -1;

		len = (size_t)(ep - cp);
		if (len && cp[len - 1] == '\n')
			len--;

		out->hash[i] = blame_line_hash(cp, len);
		for (; cp < ep; cp++) {
			unsigned char ch = *((const unsigned char *)cp);
			if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			    (ch >= '0' && ch <= '9'))
				score++;
		}
		out->score[i] = score;
	}

	out->count = e->num_lines;
	return 0;
}

static void blame_entry_lines_free(blame_entry_lines *l)
{
	git__free(l->hash);
	git__free(l->score);
}

/*
 * Whether the xdiff of the lines `l` against the source `s` can give a
 * split that scores above `threshold`.  A split is one run of lines that
 * the two files have in common, in the same order; the score of a split
 * is 1 plus the letters and digits of its lines (see blame_entry_score).
 * Each line of a run of two or more lines is in `s` together with the
 * line after it, and a run of one line is in `s`.  So the best score of
 * a run in which each pair of lines is in the filter, and of a single
 * line that is in the filter, is an upper bound.  The filter has a
 * single line only if it scores above BLAME_FILTER_LINE_SCORE; thus for
 * a lower threshold the answer is always "can match".
 */
static bool blame_source_can_match(
		const blame_source *s,
		const blame_entry_lines *l,
		unsigned threshold)
{
	size_t k, run_len = 0;
	unsigned long run = 0;

	if (threshold < BLAME_FILTER_LINE_SCORE)
		return true;

	for (k = 0; k < l->count; k++) {
		if (k > 0 && run_len > 0 &&
		    blame_filter_test(s, blame_item_pair(l->hash[k - 1],
				l->hash[k]))) {
			run += l->score[k];
			run_len++;
		} else {
			run = l->score[k];
			run_len = 1;
		}

		if (run_len >= 2 && 1 + run > threshold)
			return true;
		if (run_len == 1 && 1 + (unsigned long)l->score[k] > threshold &&
		    blame_filter_test(s, blame_item_line(l->hash[k])))
			return true;
	}

	return false;
}

/*
 * Find lines that `target` is still suspected for anywhere in the blob of
 * `parent`, the same file in a parent commit: this detects lines that
 * were moved or copied within the file.  An entry is split when a run of
 * common lines scores above the move threshold; the parts of the entry
 * that are left are searched again.
 *
 * Returns 1 when `target` is not suspected for any line, 0 when it
 * still is, and a negative error code on failure.
 */
static int find_move_in_parent(
		git_blame *blame,
		git_blame__origin *target,
		git_blame__origin *parent)
{
	unsigned threshold = blame_move_score(blame);
	git_blame__entry *e, split[3];
	blame_source filter;
	size_t last_in_target;
	mmfile_t file_p;
	int error = 0;

	if (!find_last_in_target(&last_in_target, blame, target))
		return 1;

	fill_origin_blob(parent, &file_p);
	if (!file_p.ptr)
		return 0;

	/*
	 * With a cache, skip the entries that the filter of the parent blob
	 * proves cannot move there (see blame_source_can_match).
	 */
	memset(&filter, 0, sizeof(filter));
	if (blame->cache &&
	    !(blame->options.flags & GIT_BLAME_IGNORE_WHITESPACE) &&
	    (size_t)file_p.size <= GIT_XDIFF_MAX_SIZE &&
	    (error = blame_filter_build(&filter, file_p.ptr,
			(size_t)file_p.size)) < 0)
		return error;

	for (e = blame->ent; e; ) {
		if (e->guilty || !same_suspect(e->suspect, target) ||
		    blame_entry_score(blame, e) <= threshold) {
			e = e->next;
			continue;
		}

		if (filter.filtered) {
			blame_entry_lines lines;
			bool can_match;

			if ((error = blame_entry_lines_init(&lines, blame, e)) < 0) {
				blame_entry_lines_free(&lines);
				break;
			}
			can_match = blame_source_can_match(&filter, &lines,
				threshold);
			blame_entry_lines_free(&lines);
			blame->cache_counts.checks++;
			if (!can_match) {
				blame->cache_counts.pruned++;
#ifdef GIT_BLAME_PROFILE
				if (prof_check_prune()) {
					memset(split, 0, sizeof(split));
					if (find_copy_in_blob(blame, e, parent, split,
							&file_p) == 0 && split[1].suspect &&
					    blame_entry_score(blame, &split[1]) > threshold) {
						prof_prune_violations++;
						fprintf(stderr, "PRUNE VIOLATION (move) %s lno=%zu n=%zu score=%u\n",
							parent->path, e->lno, e->num_lines,
							blame_entry_score(blame, &split[1]));
					}
					decref_split(split);
				}
#endif
				e = e->next;
				continue;
			}
			blame->cache_counts.xdiffs++;
		}

		memset(split, 0, sizeof(split));

		{
			PROF_VARS;
			PROF_START();
			error = find_copy_in_blob(blame, e, parent, split, &file_p);
			PROF_STOP(P_MOVE_XDIFF);
		}

		if (!error && split[1].suspect &&
		    blame_entry_score(blame, &split[1]) > threshold) {
			/*
			 * `e` now holds the part before the moved lines, or the
			 * moved lines themselves.  Look at it again; the part
			 * after them follows in the list.
			 */
			error = split_blame(blame, split, e, NULL);
		} else {
			e = e->next;
		}

		decref_split(split);

		if (error < 0)
			break;
	}

	git__free(filter.bits);
	return error < 0 ? error : 0;
}

/* A blame entry that may have been copied, and its best split so far. */
typedef struct {
	git_blame__entry *ent;
	git_blame__entry split[3];
} blame_copy_candidate;

typedef git_array_t(blame_copy_candidate) blame_copy_candidates;

/* Add `e` to the candidates, if it scores above the copy threshold. */
static int add_copy_candidate(
		blame_copy_candidates *candidates,
		git_blame *blame,
		git_blame__entry *e)
{
	blame_copy_candidate *c;

	if (blame_entry_score(blame, e) <= blame_copy_score(blame))
		return 0;

	c = git_array_alloc(*candidates);
	GIT_ERROR_CHECK_ALLOC(c);

	memset(c, 0, sizeof(*c));
	c->ent = e;
	return 0;
}

static void clear_copy_candidates(blame_copy_candidates *candidates)
{
	blame_copy_candidate *c;
	size_t i;

	git_array_foreach(*candidates, i, c)
		decref_split(c->split);

	git_array_clear(*candidates);
}

/*
 * List the files of `parent` to search for copied lines: the files that
 * were modified or deleted between `parent` and the commit of `target`.
 * To find copies harder, list the files that were not modified, too:
 * always with `GIT_BLAME_TRACK_COPIES_ANY_COMMIT_COPIES`, and with
 * `GIT_BLAME_TRACK_COPIES_SAME_COMMIT_COPIES` when the commit created or
 * renamed the file of `target`.
 */
static int diff_copy_sources(
		git_diff **out,
		git_blame *blame,
		git_blame__origin *target,
		git_commit *parent,
		git_blame__origin *porigin,
		uint32_t opt)
{
	git_diff_options diffopts = GIT_DIFF_OPTIONS_INIT;
	git_tree *otree = NULL, *ptree = NULL;
	int error;

	diffopts.context_lines = 0;
	diffopts.flags = GIT_DIFF_SKIP_BINARY_CHECK;

	if ((opt & GIT_BLAME_TRACK_COPIES_ANY_COMMIT_COPIES) ||
	    ((opt & GIT_BLAME_TRACK_COPIES_SAME_COMMIT_COPIES) &&
	     (!porigin || strcmp(target->path, porigin->path))))
		diffopts.flags |= GIT_DIFF_INCLUDE_UNMODIFIED;

	if ((error = git_commit_tree(&otree, target->commit)) < 0 ||
	    (error = git_commit_tree(&ptree, parent)) < 0)
		goto done;

	error = git_diff_tree_to_tree(out, blame->repository, ptree, otree,
		&diffopts);

done:
	git_tree_free(otree);
	git_tree_free(ptree);
	return error;
}

/*
 * Whether the old side of `delta` is a file of the parent to search for
 * copied lines.  The file that `find_move_in_parent` has searched already,
 * and submodules, are not.
 */
static bool is_copy_source(
		const git_diff_delta *delta,
		git_blame__origin *porigin)
{
	if (delta->status == GIT_DELTA_ADDED ||
	    delta->old_file.mode == GIT_FILEMODE_UNREADABLE ||
	    delta->old_file.mode == GIT_FILEMODE_TREE ||
	    delta->old_file.mode == GIT_FILEMODE_COMMIT)
		return false;

	return !porigin || strcmp(delta->old_file.path, porigin->path) != 0;
}

/* Create an origin for the old side of `delta`, a file of `parent`. */
static int make_source_origin(
		git_blame__origin **out,
		git_blame *blame,
		git_commit *parent,
		const git_diff_delta *delta)
{
	git_commit *commit = NULL;
	git_blob *blob = NULL;
	int error;

	if ((error = git_commit_dup(&commit, parent)) < 0 ||
	    (error = git_blob_lookup(&blob, blame->repository,
			&delta->old_file.id)) < 0 ||
	    (error = new_origin(out, commit, blob, delta->old_file.path,
			delta->old_file.mode)) < 0) {
		git_blob_free(blob);
		git_commit_free(commit);
	}

	return error;
}

/*
 * Search every source file for the best match of every candidate, and
 * record it in the candidate.
 */
static int find_best_copies_in_diff(
		git_blame *blame,
		blame_copy_candidates *candidates,
		git_commit *parent,
		git_diff *diff,
		git_blame__origin *porigin)
{
	git_blame__entry potential[3];
	git_blame__origin *norigin = NULL;
	blame_copy_candidate *c;
	mmfile_t file_p;
	size_t i, j;
	int error = 0;
	PROF_VARS;

	for (i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);

		if (!is_copy_source(delta, porigin))
			continue;

		PROF_START();
		error = make_source_origin(&norigin, blame, parent, delta);
		PROF_STOP(P_COPY_LOAD);
		if (error < 0)
			return error;
		PROF_COUNT(prof_copy_sources, 1);
		PROF_COUNT(prof_copy_bytes, git_blob_rawsize(norigin->blob));

		fill_origin_blob(norigin, &file_p);

		git_array_foreach(*candidates, j, c) {
			memset(potential, 0, sizeof(potential));

			PROF_START();
			error = find_copy_in_blob(blame, c->ent, norigin,
					potential, &file_p);
			PROF_STOP(P_COPY_XDIFF);
			PROF_COUNT(prof_copy_xdiffs, 1);
#ifdef GIT_BLAME_PROFILE
			if (!error && potential[1].suspect &&
			    blame_entry_score(blame, &potential[1]) >
			    blame_copy_score(blame))
				prof_copy_useful++;
#endif
			if (error == 0)
				copy_split_if_better(blame, c->split, potential);

			decref_split(potential);

			if (error < 0)
				break;
		}

		origin_decref(norigin);

		if (error < 0)
			return error;
	}

	return 0;
}

/*
 * The cache that many blames of one repository share (experimental).
 *
 * The search for copied lines in a commit compares the lines of the
 * target with every file that the commit changed, and it does so for
 * each target file that the commit changed.  In a commit that changes
 * 50,000 files, each of those files reads 50,000 blobs again.
 *
 * The cache keeps, for each (commit, parent) pair, the list of the copy
 * sources, and for each source a filter of its lines: a Bloom filter of
 * the hashes of its lines and of each pair of lines that follow one
 * another.  A run of lines that the target and the source have in common
 * is in the filter; so when no run of the target lines that the filter
 * can have scores above the copy threshold, the xdiff of that source
 * cannot give a split above the threshold, and the search skips it.  The
 * filter can have false positives but no false negatives, thus the
 * result does not change.
 */

static void blame_source_list_free(blame_source_list *list)
{
	size_t i;

	if (!list)
		return;

	for (i = 0; i < list->count; i++) {
		git__free(list->sources[i].bits);
		git__free(list->sources[i].path);
	}
	git__free(list->sources);
	git__free(list->key);
	git__free(list);
}

/*
 * Remove the least recently used list that no blame uses now.  The
 * caller holds the lock.
 */
static bool blame_cache_evict(git_blame_cache *cache)
{
	git_hashmap_iter_t iter = GIT_HASHMAP_ITER_INIT;
	blame_source_list *list, *lru = NULL;

	while (blame_source_listmap_iterate(&iter, NULL, &list,
			&cache->lists) == 0) {
		if (list->ready && !list->refcount &&
		    (!lru || list->last_use < lru->last_use))
			lru = list;
	}

	if (!lru)
		return false;

	blame_source_listmap_remove(&cache->lists, lru->key);
	cache->stats.bytes -= lru->bytes;
	cache->stats.lists--;
	cache->stats.list_evictions++;
	blame_source_list_free(lru);
	return true;
}

/*
 * Make the list of sources, and the filter of each source: read every
 * blob once.  This runs without the lock; `list` is not visible to
 * other blames yet.
 */
static int blame_source_list_build(
		blame_source_list *list,
		git_blame *blame,
		git_blame__origin *target,
		git_commit *parent,
		git_blame__origin *porigin,
		uint32_t opt,
		bool filters)
{
	git_diff *diff = NULL;
	size_t i;
	int error;
	PROF_VARS;

	if ((error = diff_copy_sources(&diff, blame, target, parent,
			porigin, opt)) < 0)
		return error;

	list->sources = git__calloc(git_diff_num_deltas(diff) + 1,
		sizeof(blame_source));
	if (!list->sources) {
		error = -1;
		goto done;
	}
	list->bytes = sizeof(*list) + strlen(list->key) + 1 +
		sizeof(blame_source);

	for (i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		blame_source *s;
		git_blob *blob = NULL;

		if (!is_copy_source(delta, NULL))
			continue;

		s = &list->sources[list->count];
		git_oid_cpy(&s->id, &delta->old_file.id);
		s->mode = delta->old_file.mode;
		if ((s->path = git__strdup(delta->old_file.path)) == NULL) {
			error = -1;
			goto done;
		}
		list->count++;
		list->bytes += sizeof(*s) + strlen(s->path) + 1;

		if (!filters) {
			s->no_filter = true;
			continue;
		}

		PROF_START();
		error = git_blob_lookup(&blob, blame->repository, &s->id);
		PROF_STOP(P_COPY_LOAD);
		if (error < 0)
			goto done;
		blame->cache_counts.blob_loads++;
		PROF_COUNT(prof_copy_sources, 1);
		PROF_COUNT(prof_copy_bytes, git_blob_rawsize(blob));

		if (git_blob_rawsize(blob) > GIT_XDIFF_MAX_SIZE) {
			s->no_filter = true;
		} else {
			memcpy(&s->salt, s->id.id, sizeof(s->salt));
			PROF_START();
			error = blame_filter_build(s, git_blob_rawcontent(blob),
				(size_t)git_blob_rawsize(blob));
			PROF_STOP(P_FILTER);
			if (error < 0) {
				git_blob_free(blob);
				goto done;
			}
			list->bytes += (size_t)s->nbits / 8;
			list->filters++;
		}
		git_blob_free(blob);
	}

done:
	git_diff_free(diff);
	return error;
}

/*
 * Get the list of the copy sources of `target` in `parent`, the same
 * files as `diff_copy_sources` lists, in the same order, and hold it
 * until blame_cache_release.  The list does not leave out the file of
 * `porigin`; the caller does.  When another blame makes the same list
 * at the same time, wait for it.
 */
static int blame_cache_sources(
		blame_source_list **out,
		git_blame *blame,
		git_blame__origin *target,
		git_commit *parent,
		git_blame__origin *porigin,
		uint32_t opt)
{
	git_blame_cache *cache = blame->cache;
	bool filters = !(blame->options.flags & GIT_BLAME_IGNORE_WHITESPACE);
	char key[GIT_OID_MAX_HEXSIZE * 2 + 5];
	blame_source_list *list = NULL;
	size_t n;
	int unmodified, error;

	unmodified = (opt & GIT_BLAME_TRACK_COPIES_ANY_COMMIT_COPIES) ||
		((opt & GIT_BLAME_TRACK_COPIES_SAME_COMMIT_COPIES) &&
		 (!porigin || strcmp(target->path, porigin->path)));

	git_oid_tostr(key, GIT_OID_MAX_HEXSIZE + 1,
		git_commit_id(target->commit));
	n = strlen(key);
	key[n++] = ' ';
	git_oid_tostr(key + n, GIT_OID_MAX_HEXSIZE + 1, git_commit_id(parent));
	n = strlen(key);
	key[n++] = ' ';
	key[n++] = unmodified ? 'u' : 'm';
	key[n++] = filters ? 'f' : 'n';
	key[n] = '\0';

	if (git_mutex_lock(&cache->lock) < 0) {
		git_error_set(GIT_ERROR_THREAD, "unable to lock the blame cache");
		return -1;
	}

	while (blame_source_listmap_get(&list, &cache->lists, key) == 0) {
		if (list->ready) {
			list->refcount++;
			list->last_use = ++cache->clock;
			cache->stats.list_hits++;
			git_mutex_unlock(&cache->lock);
			*out = list;
			return 0;
		}
		/* Another blame makes this list now. */
		git_cond_wait(&cache->ready, &cache->lock);
	}

	/* Make the list; other blames that want it wait. */
	list = git__calloc(1, sizeof(*list));
	if (!list || (list->key = git__strdup(key)) == NULL ||
	    blame_source_listmap_put(&cache->lists, list->key, list) < 0) {
		if (list)
			git__free(list->key);
		git__free(list);
		git_mutex_unlock(&cache->lock);
		git_error_set_oom();
		return -1;
	}
	git_mutex_unlock(&cache->lock);

	error = blame_source_list_build(list, blame, target, parent, porigin,
		opt, filters);

	git_mutex_lock(&cache->lock);
	if (error < 0) {
		blame_source_listmap_remove(&cache->lists, list->key);
		blame_source_list_free(list);
		list = NULL;
	} else {
		/* The new list stays, even when it alone is above the limit. */
		while (cache->stats.bytes + list->bytes > cache->stats.max_bytes &&
		       blame_cache_evict(cache))
			;
		list->ready = true;
		list->refcount = 1;
		list->last_use = ++cache->clock;
		cache->stats.bytes += list->bytes;
		cache->stats.lists++;
		cache->stats.list_misses++;
		cache->stats.filters_built += list->filters;
	}
	git_cond_broadcast(&cache->ready);
	git_mutex_unlock(&cache->lock);

	*out = list;
	return error;
}

static void blame_cache_release(git_blame_cache *cache, blame_source_list *list)
{
	if (!list)
		return;
	git_mutex_lock(&cache->lock);
	list->refcount--;
	git_mutex_unlock(&cache->lock);
}

void git_blame__cache_merge_counts(git_blame *blame)
{
	git_blame_cache *cache = blame->cache;

	if (!cache)
		return;
	git_mutex_lock(&cache->lock);
	cache->stats.source_checks += blame->cache_counts.checks;
	cache->stats.source_pruned += blame->cache_counts.pruned;
	cache->stats.source_xdiffs += blame->cache_counts.xdiffs;
	cache->stats.blob_loads += blame->cache_counts.blob_loads;
	git_mutex_unlock(&cache->lock);
	memset(&blame->cache_counts, 0, sizeof(blame->cache_counts));
}

/*
 * Like find_best_copies_in_diff, for a list from the cache.  The list
 * is read only here, so blames in other threads can use it too.
 *
 * find_best_copies_in_diff keeps, for each candidate, the split with
 * the highest score, and of those the one of the last source (a later
 * split with the same score wins).  Here the search skips a (candidate,
 * source) pair when the filter proves that the source cannot give a
 * split above the threshold, or a split that scores at least as high as
 * the best split of the candidate so far.  The sources come in the same
 * order, so the result is the same.
 */
static int find_best_copies_in_list(
		git_blame *blame,
		blame_copy_candidates *candidates,
		git_commit *parent,
		blame_source_list *list,
		git_blame__origin *porigin)
{
	unsigned threshold = blame_copy_score(blame);
	bool prune = !(blame->options.flags & GIT_BLAME_IGNORE_WHITESPACE);
	blame_entry_lines *lines = NULL;
	unsigned *best = NULL;
	git_blame__entry potential[3];
	blame_copy_candidate *c;
	size_t i, j, ncand = git_array_size(*candidates);
	int error = 0;
	PROF_VARS;

	lines = git__calloc(ncand ? ncand : 1, sizeof(*lines));
	best = git__calloc(ncand ? ncand : 1, sizeof(*best));
	if (!lines || !best) {
		error = -1;
		goto done;
	}
	if (prune) {
		git_array_foreach(*candidates, j, c) {
			if ((error = blame_entry_lines_init(&lines[j], blame,
					c->ent)) < 0)
				goto done;
		}
	}

	for (i = 0; i < list->count; i++) {
		const blame_source *s = &list->sources[i];
		git_blame__origin *norigin = NULL;
		mmfile_t file_p;

		if (porigin && !strcmp(s->path, porigin->path))
			continue;

		git_array_foreach(*candidates, j, c) {
			unsigned bar = threshold;

			/* A later split with the same score wins: keep a tie. */
			if (c->split[1].suspect && best[j] > bar)
				bar = best[j] - 1;

			blame->cache_counts.checks++;
			if (prune && s->filtered &&
			    !blame_source_can_match(s, &lines[j], bar)) {
				blame->cache_counts.pruned++;
				continue;
			}

			if (!norigin) {
				git_commit *commit = NULL;
				git_blob *blob = NULL;

				PROF_START();
				error = git_blob_lookup(&blob,
					blame->repository, &s->id);
				PROF_STOP(P_COPY_LOAD);
				if (error < 0)
					break;
				blame->cache_counts.blob_loads++;
				PROF_COUNT(prof_copy_sources, 1);
				PROF_COUNT(prof_copy_bytes, git_blob_rawsize(blob));

				if ((error = git_commit_dup(&commit, parent)) < 0 ||
				    (error = new_origin(&norigin, commit, blob,
						s->path, s->mode)) < 0) {
					git_commit_free(commit);
					git_blob_free(blob);
					break;
				}
				fill_origin_blob(norigin, &file_p);
			}

			memset(potential, 0, sizeof(potential));
			blame->cache_counts.xdiffs++;

			PROF_START();
			error = find_copy_in_blob(blame, c->ent, norigin,
					potential, &file_p);
			PROF_STOP(P_COPY_XDIFF);
			PROF_COUNT(prof_copy_xdiffs, 1);

			if (error == 0 && potential[1].suspect) {
#ifdef GIT_BLAME_PROFILE
				unsigned score = blame_entry_score(blame,
					&potential[1]);
				if (score > threshold)
					prof_copy_useful++;
				if (prune && s->filtered && score > bar &&
				    !blame_source_can_match(s, &lines[j], score - 1)) {
					prof_prune_violations++;
					fprintf(stderr, "BOUND VIOLATION %s score=%u\n",
						s->path, score);
				}
#endif
				copy_split_if_better(blame, c->split, potential);
				best[j] = blame_entry_score(blame, &c->split[1]);
			}

			decref_split(potential);

			if (error < 0)
				break;
		}

		origin_decref(norigin);

		if (error < 0)
			goto done;
	}

done:
	if (lines) {
		for (j = 0; j < ncand; j++)
			blame_entry_lines_free(&lines[j]);
	}
	git__free(lines);
	git__free(best);
	return error;
}

int git_blame_cache_new(
		git_blame_cache **out,
		git_repository *repo,
		size_t max_bytes)
{
	git_blame_cache *cache;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(repo);

	cache = git__calloc(1, sizeof(*cache));
	GIT_ERROR_CHECK_ALLOC(cache);

	if (git_mutex_init(&cache->lock) < 0 ||
	    git_cond_init(&cache->ready) < 0) {
		git_error_set(GIT_ERROR_THREAD, "unable to init the blame cache");
		git__free(cache);
		return -1;
	}

	cache->repo = repo;
	cache->stats.max_bytes = max_bytes ? max_bytes : BLAME_CACHE_DEFAULT_BYTES;
	*out = cache;
	return 0;
}

void git_blame_cache_free(git_blame_cache *cache)
{
	git_hashmap_iter_t iter = GIT_HASHMAP_ITER_INIT;
	blame_source_list *list;

	if (!cache)
		return;

	while (blame_source_listmap_iterate(&iter, NULL, &list,
			&cache->lists) == 0)
		blame_source_list_free(list);

	blame_source_listmap_dispose(&cache->lists);
	git_cond_free(&cache->ready);
	git_mutex_free(&cache->lock);
	git__free(cache);
}

void git_blame_cache_get_stats(
		git_blame_cache_stats *out,
		const git_blame_cache *cache)
{
	git_blame_cache *c = (git_blame_cache *)cache;

	if (!out || !cache)
		return;
	git_mutex_lock(&c->lock);
	memcpy(out, &c->stats, sizeof(*out));
	git_mutex_unlock(&c->lock);
}

static int find_best_copies(
		git_blame *blame,
		blame_copy_candidates *candidates,
		git_commit *parent,
		git_diff *diff,
		blame_source_list *list,
		git_blame__origin *porigin)
{
	if (list)
		return find_best_copies_in_list(blame, candidates, parent,
			list, porigin);

	return find_best_copies_in_diff(blame, candidates, parent, diff,
		porigin);
}

/*
 * Find lines that `target` is still suspected for in the files of `parent`
 * (a parent of the commit of `target`) other than `porigin`: this detects
 * lines that were moved or copied from other files.  Each candidate entry
 * goes to the file with the best match, if that match scores above the
 * copy threshold; the parts of the entry that are left are searched again.
 *
 * Returns 1 when no line that `target` is suspected for is worth a
 * search anymore, 0 when some still are, and a negative error code on
 * failure.
 */
static int find_copy_in_parent(
		git_blame *blame,
		git_blame__origin *target,
		git_commit *parent,
		git_blame__origin *porigin,
		uint32_t opt)
{
	blame_copy_candidates candidates = GIT_ARRAY_INIT, next = GIT_ARRAY_INIT;
	unsigned threshold = blame_copy_score(blame);
	blame_copy_candidate *c;
	git_blame__entry *e, *tail;
	git_diff *diff = NULL;
	blame_source_list *list = NULL;
	size_t i;
	int error = 0;
#ifdef GIT_BLAME_PROFILE
	double c0, d0 = prof_t[P_COPY_DIFF], ld0 = prof_t[P_COPY_LOAD],
		x0 = prof_t[P_COPY_XDIFF];
	size_t s0 = prof_copy_sources, b0 = prof_copy_bytes, ncand, rounds = 0;
	PROF_VARS;
#endif

	for (e = blame->ent; e; e = e->next) {
		if (e->guilty || !same_suspect(e->suspect, target))
			continue;
		if ((error = add_copy_candidate(&candidates, blame, e)) < 0)
			goto done;
	}

	if (!git_array_size(candidates)) {
		error = 1;
		goto done;
	}

#ifdef GIT_BLAME_PROFILE
	c0 = prof_now();
	ncand = git_array_size(candidates);
	PROF_START();
#endif
	if (blame->cache)
		error = blame_cache_sources(&list, blame, target, parent,
			porigin, opt);
	else
		error = diff_copy_sources(&diff, blame, target, parent,
			porigin, opt);
#ifdef GIT_BLAME_PROFILE
	PROF_STOP(P_COPY_DIFF);
	prof_copy_searches++;
	if (diff)
		prof_copy_deltas += git_diff_num_deltas(diff);
#endif
	if (error < 0)
		goto done;

	while (git_array_size(candidates)) {
#ifdef GIT_BLAME_PROFILE
		rounds++;
#endif
		if ((error = find_best_copies(blame, &candidates, parent,
				diff, list, porigin)) < 0)
			goto done;

		git_array_foreach(candidates, i, c) {
			if (!c->split[1].suspect ||
			    blame_entry_score(blame, &c->split[1]) <= threshold)
				continue;

			if ((error = split_blame(blame, c->split, c->ent,
					&tail)) < 0)
				goto done;

			/* Search the parts that are left again. */
			if (c->split[0].suspect)
				error = add_copy_candidate(&next, blame, c->ent);
			if (!error && tail)
				error = add_copy_candidate(&next, blame, tail);
			if (error < 0)
				goto done;
		}

		clear_copy_candidates(&candidates);
		candidates = next;
		git_array_init(next);
	}

#ifdef GIT_BLAME_PROFILE
	if (prof_log()) {
		char a[GIT_OID_MAX_HEXSIZE + 1], b[GIT_OID_MAX_HEXSIZE + 1];
		git_oid_tostr(a, sizeof(a), git_commit_id(target->commit));
		git_oid_tostr(b, sizeof(b), git_commit_id(parent));
		fprintf(prof_log(),
			"C %s %s %d %zu %zu %zu %zu %zu %.6f %.6f %.6f %.6f\n",
			a, b, (int)!!(opt & (GIT_BLAME_TRACK_COPIES_ANY_COMMIT_COPIES |
			GIT_BLAME_TRACK_COPIES_SAME_COMMIT_COPIES)),
			diff ? git_diff_num_deltas(diff) : (list ? list->count : 0),
			prof_copy_sources - s0,
			prof_copy_bytes - b0, ncand, rounds,
			prof_now() - c0, prof_t[P_COPY_DIFF] - d0,
			prof_t[P_COPY_LOAD] - ld0, prof_t[P_COPY_XDIFF] - x0);
	}
#endif

	/* Is any line left that is worth a search in another parent? */
	error = 1;
	for (e = blame->ent; e; e = e->next) {
		if (!e->guilty && same_suspect(e->suspect, target) &&
		    blame_entry_score(blame, e) > threshold) {
			error = 0;
			break;
		}
	}

done:
	clear_copy_candidates(&candidates);
	clear_copy_candidates(&next);
	git_diff_free(diff);
	if (list)
		blame_cache_release(blame->cache, list);
	return error;
}

static int pass_blame(git_blame *blame, git_blame__origin *origin, uint32_t opt)
{
	git_commit *commit = origin->commit;
	int i, pass, num_parents;
	git_blame__origin *sg_buf[16];
	git_blame__origin *porigin, **sg_origin = sg_buf;
	int ret, error = 0;

	num_parents = git_commit_parentcount(commit);
	if (!git_oid_cmp(git_commit_id(commit), &blame->options.oldest_commit))
		/* Stop at oldest specified commit */
		num_parents = 0;
	else if (opt & GIT_BLAME_FIRST_PARENT && num_parents > 1)
		/* Limit search to the first parent */
		num_parents = 1;

	if (!num_parents) {
		git_oid_cpy(&blame->options.oldest_commit, git_commit_id(commit));
		goto finish;
	} else if (num_parents < (int)ARRAY_SIZE(sg_buf))
		memset(sg_buf, 0, sizeof(sg_buf));
	else {
		sg_origin = git__calloc(num_parents, sizeof(*sg_origin));
		GIT_ERROR_CHECK_ALLOC(sg_origin);
	}

	/*
	 * The first pass looks for the same path in the parents, the second
	 * pass looks for renames in the parents that do not have it.
	 */
	for (pass = 0; pass < 2; pass++) {
		for (i=0; i<num_parents; i++) {
			git_commit *p;
			int j, same;

			if (sg_origin[i])
				continue;

			if ((error = git_commit_parent(&p, commit, i)) < 0)
				goto finish;

			if (pass == 0) {
				PROF_VARS;
				PROF_START();
				error = find_origin(&porigin, blame, p, origin);
				PROF_STOP(P_FIND_ORIGIN);
			} else {
				error = find_rename(&porigin, blame, p, origin);
			}

			if (error < 0 || !porigin) {
				/*
				 * We only have to decrement the parent's
				 * reference count when no porigin has
				 * been created, as otherwise the commit
				 * is assigned to the created object.
				 */
				git_commit_free(p);

				if (error < 0)
					goto finish;

				continue;
			}
			if (porigin->blob && origin->blob &&
			    !git_oid_cmp(git_blob_id(porigin->blob),
					 git_blob_id(origin->blob))) {
				error = pass_whole_blame(blame, origin, porigin);
				origin_decref(porigin);
				goto finish;
			}
			for (j = same = 0; j<i; j++)
				if (sg_origin[j] &&
				    !git_oid_cmp(git_blob_id(sg_origin[j]->blob),
						 git_blob_id(porigin->blob))) {
					same = 1;
					break;
				}
			if (!same)
				sg_origin[i] = porigin;
			else
				origin_decref(porigin);
		}
	}

	/* Standard blame */
	for (i=0; i<num_parents; i++) {
		git_blame__origin *porigin = sg_origin[i];
		if (!porigin)
			continue;
		if (!origin->previous) {
			origin_incref(porigin);
			origin->previous = porigin;
		}

		if ((ret = pass_blame_to_parent(blame, origin, porigin)) != 0) {
			if (ret < 0)
				error = -1;

			goto finish;
		}
	}

	/*
	 * Optionally find moves in parents' files.
	 */
	if (opt & GIT_BLAME_TRACK_COPIES_SAME_FILE) {
		for (i = 0; i < num_parents; i++) {
			if (!sg_origin[i])
				continue;

			ret = find_move_in_parent(blame, origin, sg_origin[i]);

			if (ret != 0) {
				if (ret < 0)
					error = ret;

				goto finish;
			}
		}
	}

	/*
	 * Optionally find copies from parents' files.
	 */
	if (opt & GIT_BLAME_TRACK_COPIES_SAME_COMMIT_MOVES) {
		for (i = 0; i < num_parents; i++) {
			git_commit *p;

			if ((error = git_commit_parent(&p, commit, i)) < 0)
				goto finish;

			ret = find_copy_in_parent(blame, origin, p,
				sg_origin[i], opt);
			git_commit_free(p);

			if (ret != 0) {
				if (ret < 0)
					error = ret;

				goto finish;
			}
		}
	}

finish:
	for (i=0; i<num_parents; i++)
		if (sg_origin[i])
			origin_decref(sg_origin[i]);
	if (sg_origin != sg_buf)
		git__free(sg_origin);
	return error;
}

/*
 * If two blame entries that are next to each other came from
 * contiguous lines in the same origin (i.e. <commit, path> pair),
 * merge them together.
 */
static void coalesce(git_blame *blame)
{
	git_blame__entry *ent, *next;

	for (ent=blame->ent; ent && (next = ent->next); ent = next) {
		if (same_suspect(ent->suspect, next->suspect) &&
		    ent->guilty == next->guilty &&
		    ent->s_lno + ent->num_lines == next->s_lno)
		{
			ent->num_lines += next->num_lines;
			ent->next = next->next;
			if (ent->next)
				ent->next->prev = ent;
			origin_decref(next->suspect);
			git__free(next);
			ent->score = 0;
			next = ent; /* again */
		}
	}
}

int git_blame__like_git(git_blame *blame, uint32_t opt)
{
	int error = 0;

	while (true) {
		git_blame__entry *ent;
		git_blame__origin *suspect = NULL;

		/* Find a suspect to break down */
		for (ent = blame->ent; !suspect && ent; ent = ent->next)
			if (!ent->guilty)
				suspect = ent->suspect;
		if (!suspect)
			break;

		/* We'll use this suspect later in the loop, so hold on to it for now. */
		origin_incref(suspect);

		if ((error = pass_blame(blame, suspect, opt)) < 0)
			break;

		/* Take responsibility for the remaining entries */
		for (ent = blame->ent; ent; ent = ent->next) {
			if (same_suspect(ent->suspect, suspect)) {
				ent->guilty = true;
				ent->is_boundary = !git_oid_cmp(
						git_commit_id(suspect->commit),
						&blame->options.oldest_commit);
			}
		}
		origin_decref(suspect);
	}

	if (!error)
		coalesce(blame);

#ifdef GIT_BLAME_PROFILE
	if (prof_log()) {
		int ph;
		fprintf(prof_log(), "T");
		for (ph = 0; ph < P_NPHASES; ph++)
			fprintf(prof_log(), " %s=%.6f/%.6f", prof_names[ph],
				prof_t[ph], prof_prep[ph]);
		fprintf(prof_log(), " copy_searches=%zu copy_deltas=%zu"
			" copy_sources=%zu copy_bytes=%zu copy_xdiffs=%zu"
			" copy_useful=%zu rename_inexact=%zu rename_srcs=%zu"
			" rename_bytes=%zu prune_violations=%zu\n", prof_copy_searches,
			prof_copy_deltas, prof_copy_sources, prof_copy_bytes,
			prof_copy_xdiffs, prof_copy_useful, prof_rename_inexact,
			prof_rename_srcs, prof_rename_bytes, prof_prune_violations);
		fflush(prof_log());
	}
#endif

	return error;
}

void git_blame__free_entry(git_blame__entry *ent)
{
	if (!ent) return;
	origin_decref(ent->suspect);
	git__free(ent);
}
