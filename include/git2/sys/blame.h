/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_sys_git_blame_h__
#define INCLUDE_sys_git_blame_h__

#include "git2/common.h"
#include "git2/types.h"
#include "git2/blame.h"

/**
 * @file git2/sys/blame.h
 * @brief Experimental: a cache that many blames of one repository share
 * @defgroup git_blame_cache Blame cache
 * @ingroup Git
 *
 * When many files of one repository are blamed with copy detection, the
 * search for copied lines in one commit is the same for each file: the
 * same list of changed files, and the same blobs.  A cache keeps that
 * work between calls.  This interface is an experiment; it can change.
 * @{
 */
GIT_BEGIN_DECL

/** A cache for many blames of one repository. */
typedef struct git_blame_cache git_blame_cache;

/** Counters of a blame cache. */
typedef struct {
	size_t bytes;            /**< memory that the cache uses now */
	size_t max_bytes;        /**< the limit of the cache */
	size_t lists;            /**< lists of copy sources in the cache */
	size_t list_hits;        /**< copy searches that found their list */
	size_t list_misses;      /**< copy searches that made their list */
	size_t list_evictions;   /**< lists removed to stay in the limit */
	size_t filters_built;    /**< line filters made from blobs */
	size_t filters_skipped;  /**< sources without a filter (no memory) */
	size_t source_checks;    /**< (entry, source) pairs looked at */
	size_t source_pruned;    /**< pairs that the filter proved empty */
	size_t source_xdiffs;    /**< pairs that were compared with xdiff */
	size_t blob_loads;       /**< blobs read for the copy search */
} git_blame_cache_stats;

/**
 * Create a cache for the blames of `repo`.  The cache must not outlive
 * the repository, and only one thread can use it at a time.
 *
 * @param out the new cache
 * @param repo the repository
 * @param max_bytes the memory limit of the cache; 0 for 512 MiB
 * @return 0 or an error code
 */
GIT_EXTERN(int) git_blame_cache_new(
	git_blame_cache **out,
	git_repository *repo,
	size_t max_bytes);

/** Free a blame cache. */
GIT_EXTERN(void) git_blame_cache_free(git_blame_cache *cache);

/** Get the counters of a blame cache. */
GIT_EXTERN(void) git_blame_cache_get_stats(
	git_blame_cache_stats *out,
	const git_blame_cache *cache);

/**
 * Like `git_blame_file`, but keep the work of the copy search in
 * `cache` for the next calls.  The result is the same as the result of
 * `git_blame_file`.
 */
GIT_EXTERN(int) git_blame_file_cached(
	git_blame **out,
	git_repository *repo,
	const char *path,
	git_blame_options *options,
	git_blame_cache *cache);

/** @} */
GIT_END_DECL

#endif
