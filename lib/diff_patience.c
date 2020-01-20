/* Implementation of the Patience Diff algorithm invented by Bram Cohen:
 * Divide a diff problem into smaller chunks by an LCS of common-unique lines. */
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

#include <assert.h>
#include <diff/diff_main.h>

#include "debug.h"

/* Set unique_here = true for all atoms that exist exactly once in this list. */
static void diff_atoms_mark_unique(struct diff_data *d, unsigned int *unique_count)
{
	struct diff_atom *i;
	unsigned int count = 0;
	diff_data_foreach_atom(i, d) {
		i->patience.unique_here = true;
		i->patience.unique_in_both = true;
		count++;
	}
	diff_data_foreach_atom(i, d) {
		struct diff_atom *j;

		if (!i->patience.unique_here)
			continue;

		diff_data_foreach_atom_from(i + 1, j, d) {
			if (diff_atom_same(i, j)) {
				if (i->patience.unique_here) {
					i->patience.unique_here = false;
					i->patience.unique_in_both = false;
					count--;
				}
				j->patience.unique_here = false;
				j->patience.unique_in_both = false;
				count--;
			}
		}
	}
	if (unique_count)
		*unique_count = count;
}

/* Mark those lines as atom->patience.unique_in_both = true that appear exactly once in each side. */
static void diff_atoms_mark_unique_in_both(struct diff_data *left, struct diff_data *right,
					   unsigned int *unique_in_both_count)
{
	/* Derive the final unique_in_both count without needing an explicit iteration. So this is just some
	 * optimiziation to save one iteration in the end. */
	unsigned int unique_in_both;

	diff_atoms_mark_unique(left, &unique_in_both);
	diff_atoms_mark_unique(right, NULL);

	debug("unique_in_both %u\n", unique_in_both);

	struct diff_atom *i;
	diff_data_foreach_atom(i, left) {
		if (!i->patience.unique_here)
			continue;
		struct diff_atom *j;
		int found_in_b = 0;
		diff_data_foreach_atom(j, right) {
			if (!diff_atom_same(i, j))
				continue;
			if (!j->patience.unique_here) {
				found_in_b = 2; /* or more */
				break;
			} else {
				found_in_b = 1;
				j->patience.pos_in_other = i;
				i->patience.pos_in_other = j;
			}
		}

		if (found_in_b == 0 || found_in_b > 1) {
			i->patience.unique_in_both = false;
			unique_in_both--;
			debug("unique_in_both %u  (%d) ", unique_in_both, found_in_b);
			debug_dump_atom(left, NULL, i);
		}
	}

	/* Still need to unmark right[*]->patience.unique_in_both for atoms that don't exist in left */
	diff_data_foreach_atom(i, right) {
		if (!i->patience.unique_here
		    || !i->patience.unique_in_both)
			continue;
		struct diff_atom *j;
		bool found_in_a = false;
		diff_data_foreach_atom(j, left) {
			if (!j->patience.unique_in_both)
				continue;
			if (!diff_atom_same(i, j))
				continue;
			found_in_a = true;
			break;
		}

		if (!found_in_a)
			i->patience.unique_in_both = false;
	}

	if (unique_in_both_count)
		*unique_in_both_count = unique_in_both;
}


/* Among the lines that appear exactly once in each side, find the longest streak that appear in both files in the same
 * order (with other stuff allowed to interleave). Use patience sort for that, as in the Patience Diff algorithm.
 * See https://bramcohen.livejournal.com/73318.html and, for a much more detailed explanation,
 * https://blog.jcoglan.com/2017/09/19/the-patience-diff-algorithm/ */
enum diff_rc diff_algo_patience(const struct diff_algo_config *algo_config, struct diff_state *state)
{
	enum diff_rc rc = DIFF_RC_ENOMEM;

	struct diff_data *left = &state->left;
	struct diff_data *right = &state->right;

	unsigned int unique_in_both_count;

	debug("\n** %s\n", __func__);

	/* Find those lines that appear exactly once in 'left' and exactly once in 'right'. */
	diff_atoms_mark_unique_in_both(left, right, &unique_in_both_count);

	debug("unique_in_both_count %u\n", unique_in_both_count);
	debug("left:\n");
	debug_dump(left);
	debug("right:\n");
	debug_dump(right);

	if (!unique_in_both_count) {
		/* Cannot apply Patience, tell the caller to use fallback_algo instead. */
		return DIFF_RC_USE_DIFF_ALGO_FALLBACK;
	}

	/* An array of Longest Common Sequence is the result of the below subscope: */
	unsigned int lcs_count = 0;
	struct diff_atom **lcs = NULL;
	struct diff_atom *lcs_tail = NULL;

	{
		/* This subscope marks the lifetime of the atom_pointers allocation */

		/* One chunk of storage for atom pointers */
		struct diff_atom **atom_pointers = recallocarray(NULL, 0, unique_in_both_count * 2, sizeof(struct diff_atom*));

		/* Half for the list of atoms that still need to be put on stacks */
		struct diff_atom **uniques = atom_pointers;

		/* Half for the patience sort state's "card stacks" -- we remember only each stack's topmost "card" */
		struct diff_atom **patience_stacks = atom_pointers + unique_in_both_count;
		unsigned int patience_stacks_count = 0;

		/* Take all common, unique items from 'left' ... */

		struct diff_atom *atom;
		struct diff_atom **uniques_end = uniques;
		diff_data_foreach_atom(atom, left) {
			if (!atom->patience.unique_in_both)
				continue;
			*uniques_end = atom;
			uniques_end++;
		}

		/* ...and sort them to the order found in 'right'.
		 * The idea is to find the leftmost stack that has a higher line number and add it to the stack's top.
		 * If there is no such stack, open a new one on the right. The line number is derived from the atom*,
		 * which are array items and hence reflect the relative position in the source file. So we got the
		 * common-uniques from 'left' and sort them according to atom->patience.pos_in_other. */
		unsigned int i;
		for (i = 0; i < unique_in_both_count; i++) {
			atom = uniques[i];
			unsigned int target_stack;

			if (!patience_stacks_count)
				target_stack = 0;
			else {
				/* binary search to find the stack to put this atom "card" on. */
				unsigned int lo = 0;
				unsigned int hi = patience_stacks_count;
				while (lo < hi) {
					unsigned int mid = (lo + hi) >> 1;
					if (patience_stacks[mid]->patience.pos_in_other < atom->patience.pos_in_other)
						lo = mid + 1;
					else
						hi = mid;
				}

				target_stack = lo;
			}

			assert(target_stack <= patience_stacks_count);
			patience_stacks[target_stack] = atom;
			if (target_stack == patience_stacks_count)
				patience_stacks_count++;

			/* Record a back reference to the next stack on the left, which will form the final longest sequence
			 * later. */
			atom->patience.prev_stack = target_stack ? patience_stacks[target_stack - 1] : NULL;

		}

		/* backtrace through prev_stack references to form the final longest common sequence */
		lcs_tail = patience_stacks[patience_stacks_count - 1];
		lcs_count = patience_stacks_count;

		/* uniques and patience_stacks are no longer needed. Backpointers are in atom->patience.prev_stack */
		free(atom_pointers);
	}

	lcs = recallocarray(NULL, 0, lcs_count, sizeof(struct diff_atom*));
	struct diff_atom **lcs_backtrace_pos = &lcs[lcs_count - 1];
	struct diff_atom *atom;
	for (atom = lcs_tail; atom; atom = atom->patience.prev_stack, lcs_backtrace_pos--) {
		assert(lcs_backtrace_pos >= lcs);
		*lcs_backtrace_pos = atom;
	}

	unsigned int i;
	if (DEBUG) {
		debug("\npatience LCS:\n");
		for (i = 0; i < lcs_count; i++) {
			debug_dump_atom(left, right, lcs[i]);
		}
	}


	/* TODO: For each common-unique line found (now listed in lcs), swallow lines upwards and downwards that are
	 * identical on each side. Requires a way to represent atoms being glued to adjacent atoms. */

	debug("\ntraverse LCS, possibly recursing:\n");

	/* Now we have pinned positions in both files at which it makes sense to divide the diff problem into smaller
	 * chunks. Go into the next round: look at each section in turn, trying to again find common-unique lines in
	 * those smaller sections. As soon as no more are found, the remaining smaller sections are solved by Myers. */
	unsigned int left_pos = 0;
	unsigned int right_pos = 0;
	for (i = 0; i <= lcs_count; i++) {
		struct diff_atom *atom;
		unsigned int left_idx;
		unsigned int right_idx;

		if (i < lcs_count) {
			atom = lcs[i];
			left_idx = diff_atom_idx(left, atom);
			right_idx = diff_atom_idx(right, atom->patience.pos_in_other);
		} else {
			atom = NULL;
			left_idx = left->atoms.len;
			right_idx = right->atoms.len;
		}

		/* 'atom' now marks an atom that matches on both sides according to patience-diff
		 * (a common-unique identical atom in both files).
		 * Handle the section before and the atom itself; the section after will be handled by the next loop
		 * iteration -- note that i loops to last element + 1 ("i <= lcs_count"), so that there will be another
		 * final iteration to pick up the last remaining items after the last LCS atom.
		 * The sections before might also be empty on left and/or right.
		 * left_pos and right_pos mark the indexes of the first atoms that have not yet been handled in the
		 * previous loop iteration.
		 * left_idx and right_idx mark the indexes of the matching atom on left and right, respectively. */

		debug("iteration %u  left_pos %u  left_idx %u  right_pos %u  right_idx %u\n",
		      i, left_pos, left_idx, right_pos, right_idx);

		struct diff_chunk *chunk;

		/* Section before the matching atom */
		struct diff_atom *left_atom = &left->atoms.head[left_pos];
		unsigned int left_section_len = left_idx - left_pos;

		struct diff_atom *right_atom = &(right->atoms.head[right_pos]);
		unsigned int right_section_len = right_idx - right_pos;

		if (left_section_len && right_section_len) {
			/* Record an unsolved chunk, the caller will apply inner_algo() on this chunk. */
			if (!diff_state_add_chunk(state, false,
						  left_atom, left_section_len,
						  right_atom, right_section_len))
				goto return_rc;
		} else if (left_section_len && !right_section_len) {
			/* Only left atoms and none on the right, they form a "minus" chunk, then. */
			if (!diff_state_add_chunk(state, true,
						  left_atom, left_section_len,
						  right_atom, 0))
				goto return_rc;
		} else if (!left_section_len && right_section_len) {
			/* No left atoms, only atoms on the right, they form a "plus" chunk, then. */
			if (!diff_state_add_chunk(state, true,
						  left_atom, 0,
						  right_atom, right_section_len))
				goto return_rc;
		}
		/* else: left_section_len == 0 and right_section_len == 0, i.e. nothing here. */

		/* The atom found to match on both sides forms a chunk of equals on each side. In the very last
		 * iteration of this loop, there is no matching atom, we were just cleaning out the remaining lines. */
		if (atom) {
			if (!diff_state_add_chunk(state, true,
						  atom, 1,
						  atom->patience.pos_in_other, 1))
				goto return_rc;
		}

		left_pos = left_idx + 1;
		right_pos = right_idx + 1;
	}
	debug("** END %s\n", __func__);

	rc = DIFF_RC_OK;

return_rc:
	free(lcs);
	return rc;
}

