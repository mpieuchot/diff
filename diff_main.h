/*	$OpenBSD$	*/

/*
 * Copyright (c) 2020 Neels Hofmeyr <neels@hofmeyr.de>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Generic infrastructure to implement various diff algorithms. */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "arraylist.h"

#ifndef MAX
#define MAX(A,B) ((A)>(B)?(A):(B))
#endif
#ifndef MIN
#define MIN(A,B) ((A)<(B)?(A):(B))
#endif

struct range {
	int start;
	int end;
};

static inline bool
range_empty(const struct range *r)
{
	return r->start == r->end;
}

static inline bool
ranges_touch(const struct range *a, const struct range *b)
{
	return (a->end >= b->start) && (a->start <= b->end);
}

static inline void
ranges_merge(struct range *a, const struct range *b)
{
	*a = (struct range){
		.start = MIN(a->start, b->start),
		.end = MAX(a->end, b->end),
	};
}

static inline int
range_len(const struct range *r)
{
	if (r == NULL)
		return 0;
	return r->end - r->start;
}

/* List of all possible return codes of a diff invocation. */
enum diff_rc {
	DIFF_RC_USE_DIFF_ALGO_FALLBACK = -1,
	DIFF_RC_OK = 0,
	DIFF_RC_ENOTSUP = ENOTSUP,
	DIFF_RC_ENOMEM = ENOMEM,
	DIFF_RC_EINVAL = EINVAL,
};

struct diff_atom {
	const uint8_t *at;
	size_t len;

	/*
	 * This hash is just a very cheap speed up for finding *mismatching*
	 * atoms. When hashes match, we still need to compare entire atoms to
	 * find out whether they are indeed identical or not.
	 */
	unsigned int hash;

	/* State for the Patience Diff algorithm */
	/* TODO: keep a separate array for the patience state */
	struct {
		bool unique_here;
		bool unique_in_both;
		struct diff_atom *pos_in_other;
		struct diff_atom *prev_stack;
		struct range identical_lines;
	} patience;
};

static inline bool
diff_atom_same(const struct diff_atom *left, const struct diff_atom *right)
{
	return left->hash == right->hash
		&& left->len == right->len
		&& memcmp(left->at, right->at, left->len) == 0;
}

/*
 * For each file, there is a "root" struct diff_data referencing the entire
 * file, which the atoms are parsed from.  In recursion of diff algorithm,
 * there may be "child" struct diff_data only referencing a subsection of
 * the file, re-using the atoms parsing. For "root" structs, atoms_allocated
 * will be nonzero, indicating that the array of atoms is owned by that
 * struct. For "child" structs, atoms_allocated == 0, to indicate that the
 * struct is referencing a subset of atoms.
 */
struct diff_data {
	const uint8_t *data;
	size_t len;
	ARRAYLIST_HEAD(, diff_atom) atoms;
	struct diff_data *root;
};

void diff_data_free(struct diff_data *diff_data);

/*
 * The atom's index in the entire file. For atoms divided by lines of text,
 * this yields the line number (starting with 0). Also works for diff_data
 * that reference only a subsection of a file, always reflecting the global
 * position in the file (and not the relative position within the subsection).
 */
#define diff_atom_root_idx(DIFF_DATA, ATOM) \
	((ATOM) ? (ATOM) - ((DIFF_DATA)->root->atoms.head) : \
	 	(DIFF_DATA)->root->atoms.len)

/*
 * The atom's index within DIFF_DATA. For atoms divided by lines of text,
 * this yields the line number (starting with0).
 */
#define diff_atom_idx(DIFF_DATA, ATOM) \
	((ATOM) ? (ATOM) - ((DIFF_DATA)->atoms.head) : (DIFF_DATA)->atoms.len)

#define foreach_diff_atom(ATOM, FIRST_ATOM, COUNT) \
	for ((ATOM) = (FIRST_ATOM); \
	     (ATOM) && ((ATOM) >= (FIRST_ATOM)) && ((ATOM) - (FIRST_ATOM) < (COUNT)); \
	     (ATOM)++)

#define diff_data_foreach_atom(ATOM, DIFF_DATA) \
	foreach_diff_atom(ATOM, (DIFF_DATA)->atoms.head, (DIFF_DATA)->atoms.len)

#define diff_data_foreach_atom_from(FROM, ATOM, DIFF_DATA) \
	for ((ATOM) = (FROM); \
	     (ATOM) && ((ATOM) >= (DIFF_DATA)->atoms.head) && ((ATOM) - (DIFF_DATA)->atoms.head < (DIFF_DATA)->atoms.len); \
	     (ATOM)++)

/*
 * A diff chunk represents a set of atoms on the left and/or a set of atoms
 * on the right.
 *
 * If solved == false:
 * The diff algorithm has divided the source file, and this is a chunk that
 * the inner_algo should run on next.
 * The lines on the left should be diffed against the lines on the right.
 * (If there are no left lines or no right lines, it implies solved == true,
 * because there is nothing to diff.)
 *
 * If solved == true:
 * If there are only left atoms, it is a chunk removing atoms from the left
 * ("a minus chunk").
 * If there are only right atoms, it is a chunk adding atoms from the right
 * ("a plus chunk").
 * If there are both left and right lines, it is a chunk of equal content on
 * both sides, and left_count == right_count:
 *
 * - foo  }
 * - bar  }-- diff_chunk{ left_start = &left.atoms.head[0], left_count = 3,
 * - baz  }            right_start = NULL, right_count = 0 }
 *   moo    }
 *   goo    }-- diff_chunk{ left_start = &left.atoms.head[3], left_count = 3,
 *   zoo    }            right_start = &right.atoms.head[0], right_count = 3 }
 *  +loo      }
 *  +roo      }-- diff_chunk{ left_start = NULL, left_count = 0,
 *  +too      }            right_start = &right.atoms.head[3], right_count = 3 }
 *
 */
struct diff_chunk {
	bool solved;
	struct diff_atom *left_start;
	unsigned int left_count;
	struct diff_atom *right_start;
	unsigned int right_count;
};

ARRAYLIST_HEAD(diff_chunk_arraylist, diff_chunk);
#define DIFF_RESULT_ALLOC_BLOCKSIZE 128

struct diff_result {
	enum diff_rc rc;
	struct diff_data left;
	struct diff_data right;
	struct diff_chunk_arraylist chunks;
};

struct diff_state {
	/* The final result passed to the original diff caller. */
	struct diff_result *result;

	/*
	 * The root diff_data is in result->left,right, these are
	 * (possibly) subsections of the root data.
	 */
	struct diff_data left;
	struct diff_data right;

	unsigned int recursion_depth_left;

	/*
	 * Remaining chunks from one diff algorithm pass, if any
	 * solved == false chunks came up.
	 */
	struct diff_chunk_arraylist temp_result;
};

struct diff_chunk *diff_state_add_chunk(struct diff_state *state, bool solved,
    struct diff_atom *left_start, unsigned int left_count,
    struct diff_atom *right_start, unsigned int right_count);

/*
 * Signature of a utility function to divide both source files into diff
 * atoms.
 * It is possible that a (future) algorithm requires both source files to
 * decide on atom split points, hence this gets both left and right to atomize
 * at the same time.
 * An example is diff_atomize_text_by_line() in diff_atomize_text.c.
 *
 * func_data: context pointer (free to be used by implementation).
 * left: struct diff_data with left->data and left->len already set up, and
 *       left->atoms to be created.
 * right: struct diff_data with right->data and right->len already set up,
 *        and right->atoms to be created.
 */
typedef enum diff_rc (*diff_atomize_func_t)(void *func_data, struct diff_data *left, struct diff_data *right);

extern enum diff_rc diff_atomize_text_by_line(void *func_data, struct diff_data *left, struct diff_data *right);

struct diff_algo_config;
typedef enum diff_rc (*diff_algo_impl_t)(const struct diff_algo_config *algo_config, struct diff_state *state);

/*
 * Form a result with all left-side removed and all right-side added,
 * i.e. no actual diff algorithm involved.
 */
enum diff_rc diff_algo_none(const struct diff_algo_config *algo_config, struct diff_state *state);

/*
 * Myers Diff tracing from the start all the way through to the end,
 * requiring quadratic amounts of memory. This can fail if the required
 * space surpasses algo_config->permitted_state_size.
 */
extern enum diff_rc diff_algo_myers(const struct diff_algo_config *algo_config, struct diff_state *state);

/*
 * Myers "Divide et Impera": tracing forwards from the start and backwards
 * from the end to find a midpoint that divides the problem into smaller
 * chunks. Requires only linear amounts of memory.
 */
extern enum diff_rc diff_algo_myers_divide(const struct diff_algo_config *algo_config, struct diff_state *state);

/*
 * Patience Diff algorithm, which divides a larger diff into smaller chunks.
 * For very specific scenarios, it may lead to a complete diff result by
 * itself, but needs a fallback algo to solve chunks that don't have
 * common-unique atoms.
 */
extern enum diff_rc diff_algo_patience(const struct diff_algo_config *algo_config, struct diff_state *state);

/* Diff algorithms to use, possibly nested. For example:
 *
 * struct diff_algo_config myers, patience, myers_divide;
 *
 * myers = (struct diff_algo_config){
 *         .impl = diff_algo_myers,
 *         .permitted_state_size = 32 * 1024 * 1024,
 *         .fallback_algo = &patience,   // when too large
 * };
 *
 * patience = (struct diff_algo_config){
 *         .impl = diff_algo_patience,
 *         .inner_algo = &patience,        // After subdivision
 *         .fallback_algo = &myers_divide, // If subdivision failed
 * };
 *
 * myers_divide = (struct diff_algo_config){
 *         .impl = diff_algo_myers_divide,
 *         .inner_algo = &myers, // When division succeeded, start from the top.
 *          .fallback_algo = NULL, // implies diff_algo_none
 * };
 *
 * struct diff_config config = {
 *         .algo = &myers,
 *         ...
 * };
 * diff_main(&config, ...);
 */
struct diff_algo_config {
	diff_algo_impl_t impl;

	/*
	 * Fail this algo if it would use more than this amount of memory,
	 * and instead use fallback_algo (diff_algo_myers).
	 *
	 * permitted_state_size == 0 means no limitation.
	 */
	size_t permitted_state_size;

	/*
	 * For algorithms that divide into smaller chunks, use this
	 * algorithm to solve the divided chunks.
	 */
	const struct diff_algo_config *inner_algo;

	/*
	 * If the algorithm fails (e.g. diff_algo_myers_if_small needs too
	 * large state, or diff_algo_patience can't find any common-unique
	 * atoms), then use this algorithm instead.
	 */
	const struct diff_algo_config *fallback_algo;
};

struct diff_config {
	diff_atomize_func_t atomize_func;
	void *atomize_func_data;

	const struct diff_algo_config *algo;

	/*
	 * How deep to step into subdivisions of a source file, a
	 * paranoia / safety measure to guard against infinite loops
	 * through diff algorithms.
	 * When the maximum recursion is reached, employ diff_algo_none
	 * (i.e. remove all left atoms and add all right atoms).
	 */
	unsigned int max_recursion_depth;
};

struct diff_result *diff_main(const struct diff_config *config,
			      const uint8_t *left_data, size_t left_len,
			      const uint8_t *right_data, size_t right_len);
void diff_result_free(struct diff_result *result);
