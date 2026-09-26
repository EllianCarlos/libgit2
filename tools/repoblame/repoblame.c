/*
 * repoblame: blame many files of one repository in one process, like
 * `lg2blame` does for one file, and share the work of the copy search
 * between the files (git_blame_file_cached).
 *
 * usage: repoblame [-M[n]] [-C[n]] [-C -C] [-C -C -C] [-w] [-n]
 *                  [-m MiB] [-j THREADS] [-r PASSES] [-o OUTDIR]
 *                  [-s STATS] <repo> <rev> <list>
 *
 * <list> has one "<idx>\t<path>" row per file ("-" reads stdin).  For
 * each file, the blame goes to OUTDIR/<idx>.txt with one line per final
 * line: "<commit> <orig_line> <orig_path>", as lg2blame prints it.
 * One row per file goes to STATS (default stderr):
 *     idx  pass  thread  wall_s  cpu_s  lines  rc  list_misses
 * and the counters of the cache at the end.  cpu_s is the CPU time of
 * the thread; list_misses is the growth of the counter during the blame
 * (with more threads, it can include the misses of other threads).
 *
 * -n turns the cache off (plain git_blame_file, same process).
 * -m sets the memory limit of the cache in MiB (default 512).
 * -j runs THREADS workers, each with its own git_repository; they share
 *    one cache, and take the files of the list in order.
 * -r blames the whole list PASSES times (the output of the last pass
 *    stays); pass 2 shows the cost with a warm cache.
 * -O sets the libgit2 object cache to MiB, and lets it keep trees and
 *    blobs up to 4 MiB (by default it keeps no blob, and no tree above
 *    4 KiB).
 * Hash verification of objects is always off, like "lg2blame -H".
 */
#define _GNU_SOURCE
#include <git2.h>
#include <git2/sys/blame.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double cpu(void)
{
	struct rusage ru;
	getrusage(RUSAGE_THREAD, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
	       ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

typedef struct {
	char *idx, *path;
} item;

static git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
static git_blame_cache *cache;
static const char *repopath, *outdir = ".";
static git_oid newest;
static item *items;
static size_t nitems, next_item, passes = 1;
static int failures;
static FILE *stats;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static size_t misses(void)
{
	git_blame_cache_stats st;

	if (!cache)
		return 0;
	git_blame_cache_get_stats(&st, cache);
	return st.list_misses;
}

static void *worker(void *arg)
{
	int thread = (int)(size_t)arg;
	git_blame_options o = opts;
	git_repository *repo;

	if (git_repository_open(&repo, repopath) < 0) {
		fprintf(stderr, "repoblame: %s\n", git_error_last()->message);
		exit(1);
	}
	git_oid_cpy(&o.newest_commit, &newest);

	for (;;) {
		git_blame *blame = NULL;
		char outname[8192];
		size_t k, pass, i, n, nlines = 0, m0;
		double t0, c0;
		FILE *out;
		int rc;

		pthread_mutex_lock(&lock);
		k = next_item++;
		pthread_mutex_unlock(&lock);
		if (k >= nitems * passes)
			break;
		pass = k / nitems + 1;
		k %= nitems;

		t0 = now();
		c0 = cpu();
		m0 = misses();
		rc = git_blame_file_cached(&blame, repo, items[k].path, &o, cache);
		snprintf(outname, sizeof(outname), "%s/%s.txt", outdir,
			items[k].idx);
		if (rc == 0 && (out = fopen(outname, "w")) != NULL) {
			for (i = 0; i < git_blame_hunkcount(blame); i++) {
				const git_blame_hunk *h =
					git_blame_hunk_byindex(blame, i);
				char hex[GIT_OID_MAX_HEXSIZE + 1];

				git_oid_tostr(hex, sizeof(hex), &h->orig_commit_id);
				for (n = 0; n < h->lines_in_hunk; n++)
					fprintf(out, "%s %zu %s\n", hex,
						h->orig_start_line_number + n,
						h->orig_path);
				nlines += h->lines_in_hunk;
			}
			fclose(out);
		} else {
			pthread_mutex_lock(&lock);
			failures++;
			pthread_mutex_unlock(&lock);
			fprintf(stderr, "repoblame: %s: %s\n", items[k].path,
				rc ? git_error_last()->message : "cannot write");
		}
		git_blame_free(blame);

		pthread_mutex_lock(&lock);
		fprintf(stats, "%s\t%zu\t%d\t%.3f\t%.3f\t%zu\t%d\t%zu\n",
			items[k].idx, pass, thread, now() - t0, cpu() - c0, nlines,
			rc, misses() - m0);
		fflush(stats);
		pthread_mutex_unlock(&lock);
	}

	git_repository_free(repo);
	return NULL;
}

static void print_stats(FILE *f, git_blame_cache *cache)
{
	git_blame_cache_stats st;

	if (!cache)
		return;
	git_blame_cache_get_stats(&st, cache);
	fprintf(f, "# cache bytes=%zu max=%zu lists=%zu hits=%zu misses=%zu "
		"evictions=%zu filters=%zu filters_skipped=%zu checks=%zu "
		"pruned=%zu xdiffs=%zu blob_loads=%zu\n",
		st.bytes, st.max_bytes, st.lists, st.list_hits,
		st.list_misses, st.list_evictions, st.filters_built,
		st.filters_skipped, st.source_checks, st.source_pruned,
		st.source_xdiffs, st.blob_loads);
}

int main(int argc, char **argv)
{
	git_repository *repo;
	git_object *obj;
	const char *statsname = NULL;
	pthread_t *threads;
	FILE *list;
	char line[8192];
	int c_count = 0, use_cache = 1, a = 1, nthreads = 1, t;
	size_t mib = 512, alloc = 0, objmib = 0;

	stats = stderr;
	for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
		const char *s = argv[a];
		if (!strncmp(s, "-M", 2)) {
			opts.flags |= GIT_BLAME_TRACK_COPIES_SAME_FILE;
			if (s[2])
				opts.min_match_characters = (uint16_t)atoi(s + 2);
		} else if (!strncmp(s, "-C", 2)) {
			c_count++;
			if (s[2])
				opts.min_match_characters = (uint16_t)atoi(s + 2);
		} else if (!strcmp(s, "-w")) {
			opts.flags |= GIT_BLAME_IGNORE_WHITESPACE;
		} else if (!strcmp(s, "-n")) {
			use_cache = 0;
		} else if (!strcmp(s, "-m") && a + 1 < argc) {
			mib = (size_t)atol(argv[++a]);
		} else if (!strcmp(s, "-j") && a + 1 < argc) {
			nthreads = atoi(argv[++a]);
		} else if (!strcmp(s, "-O") && a + 1 < argc) {
			objmib = (size_t)atol(argv[++a]);
		} else if (!strcmp(s, "-r") && a + 1 < argc) {
			passes = (size_t)atol(argv[++a]);
		} else if (!strcmp(s, "-o") && a + 1 < argc) {
			outdir = argv[++a];
		} else if (!strcmp(s, "-s") && a + 1 < argc) {
			statsname = argv[++a];
		} else {
			fprintf(stderr, "repoblame: unknown option %s\n", s);
			return 2;
		}
	}
	if (c_count == 1)
		opts.flags |= GIT_BLAME_TRACK_COPIES_SAME_COMMIT_MOVES;
	else if (c_count == 2)
		opts.flags |= GIT_BLAME_TRACK_COPIES_SAME_COMMIT_COPIES;
	else if (c_count >= 3)
		opts.flags |= GIT_BLAME_TRACK_COPIES_ANY_COMMIT_COPIES;

	if (argc - a != 3 || nthreads < 1 || passes < 1) {
		fprintf(stderr, "usage: repoblame [opts] <repo> <rev> <list>\n");
		return 2;
	}
	repopath = argv[a];

	git_libgit2_init();
	git_libgit2_opts(GIT_OPT_ENABLE_STRICT_HASH_VERIFICATION, 0);
	if (objmib) {
		git_libgit2_opts(GIT_OPT_SET_CACHE_MAX_SIZE,
			(ssize_t)(objmib << 20));
		git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT,
			GIT_OBJECT_TREE, (size_t)4 << 20);
		git_libgit2_opts(GIT_OPT_SET_CACHE_OBJECT_LIMIT,
			GIT_OBJECT_BLOB, (size_t)4 << 20);
	}
	if (git_repository_open(&repo, repopath) < 0 ||
	    git_revparse_single(&obj, repo, argv[a + 1]) < 0) {
		fprintf(stderr, "repoblame: %s\n", git_error_last()->message);
		return 1;
	}
	git_oid_cpy(&newest, git_object_id(obj));

	if (use_cache && git_blame_cache_new(&cache, repo, mib << 20) < 0) {
		fprintf(stderr, "repoblame: %s\n", git_error_last()->message);
		return 1;
	}

	list = strcmp(argv[a + 2], "-") ? fopen(argv[a + 2], "r") : stdin;
	if (!list) {
		perror(argv[a + 2]);
		return 1;
	}
	while (fgets(line, sizeof(line), list)) {
		char *tab = strchr(line, '\t'), *nl;

		if (!tab)
			continue;
		*tab = '\0';
		if ((nl = strchr(tab + 1, '\n')) != NULL)
			*nl = '\0';
		if (nitems == alloc) {
			alloc = alloc ? alloc * 2 : 256;
			items = realloc(items, alloc * sizeof(*items));
		}
		items[nitems].idx = strdup(line);
		items[nitems].path = strdup(tab + 1);
		nitems++;
	}

	if (statsname && !(stats = fopen(statsname, "w"))) {
		perror(statsname);
		return 1;
	}
	fprintf(stats, "idx\tpass\tthread\twall_s\tcpu_s\tlines\trc\tlist_misses\n");

	threads = calloc((size_t)nthreads, sizeof(*threads));
	for (t = 0; t < nthreads; t++)
		pthread_create(&threads[t], NULL, worker, (void *)(size_t)t);
	for (t = 0; t < nthreads; t++)
		pthread_join(threads[t], NULL);

	print_stats(stats, cache);
	git_blame_cache_free(cache);
	git_object_free(obj);
	git_repository_free(repo);
	git_libgit2_shutdown();
	return failures ? 1 : 0;
}
