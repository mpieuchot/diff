/* Generic infrastructure to implement various diff algorithms (implementation). */
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


#include <sys/queue.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include <assert.h>

#include "diff_main.h"
#include "debug.h"

/* Even if a left or right side is empty, diff output may need to know the position in that file.
 * So left_start or right_start must never be NULL -- pass left_count or right_count as zero to indicate staying at that
 * position without consuming any lines. */
struct diff_chunk *diff_state_add_chunk(struct diff_state *state, bool solved,
					struct diff_atom *left_start, unsigned int left_count,
					struct diff_atom *right_start, unsigned int right_count)
{
	struct diff_chunk *chunk;
	diff_chunk_arraylist_t *result;

	if (solved && !state->temp_result.len)
		result = &state->result->chunks;
	else
		result = &state->temp_result;

	ARRAYLIST_ADD(chunk, *result);
	if (!chunk)
		return NULL;
	*chunk = (struct diff_chunk){
		.solved = solved,
		.left_start = left_start,
		.left_count = left_count,
		.right_start = right_start,
		.right_count = right_count,
	};

	debug("Add %s chunk:\n", chunk->solved ? "solved" : "UNSOLVED");
	debug("L\n");
	debug_dump_atoms(&state->left, chunk->left_start, chunk->left_count);
	debug("R\n");
	debug_dump_atoms(&state->right, chunk->right_start, chunk->right_count);
	return chunk;
}

void diff_data_init_root(struct diff_data *d, const uint8_t *data, size_t len)
{
	*d = (struct diff_data){
		.data = data,
		.len = len,
		.root = d,
	};
}

void diff_data_init_subsection(struct diff_data *d, struct diff_data *parent,
			       struct diff_atom *from_atom, unsigned int atoms_count)
{
	struct diff_atom *last_atom = from_atom + atoms_count - 1;
	*d = (struct diff_data){
		.data = from_atom->at,
		.len = (last_atom->at + last_atom->len) - from_atom->at,
		.root = parent->root,
		.atoms.head = from_atom,
		.atoms.len = atoms_count,
	};

	debug("subsection:\n");
	debug_dump(d);
}

void diff_data_free(struct diff_data *diff_data)
{
	if (!diff_data)
		return;
	if (diff_data->atoms.allocated)
		ARRAYLIST_FREE(diff_data->atoms);
}

enum diff_rc diff_algo_none(const struct diff_algo_config *algo_config, struct diff_state *state)
{
	debug("\n** %s\n", __func__);
	debug("left:\n");
	debug_dump(&state->left);
	debug("right:\n");
	debug_dump(&state->right);
	debug_dump_myers_graph(&state->left, &state->right, NULL);

	/* Add a chunk of equal lines, if any */
	unsigned int equal_atoms = 0;
	while (equal_atoms < state->left.atoms.len && equal_atoms < state->right.atoms.len
	       && diff_atom_same(&state->left.atoms.head[equal_atoms], &state->right.atoms.head[equal_atoms]))
		equal_atoms++;
	if (equal_atoms) {
		if (!diff_state_add_chunk(state, true,
					  &state->left.atoms.head[0], equal_atoms,
					  &state->right.atoms.head[0], equal_atoms))
			return DIFF_RC_ENOMEM;
	}

	/* Add a "minus" chunk with all lines from the left. */
	if (equal_atoms < state->left.atoms.len) {
		if (!diff_state_add_chunk(state, true,
					  &state->left.atoms.head[equal_atoms],
					  state->left.atoms.len - equal_atoms,
					  NULL, 0))
		    return DIFF_RC_ENOMEM;
	}

	/* Add a "plus" chunk with all lines from the right. */
	if (equal_atoms < state->right.atoms.len) {
		if (!diff_state_add_chunk(state, true,
					  NULL, 0,
					  &state->right.atoms.head[equal_atoms],
					  state->right.atoms.len - equal_atoms))
		return DIFF_RC_ENOMEM;
	}
	return DIFF_RC_OK;
}

enum diff_rc diff_run_algo(const struct diff_algo_config *algo_config, struct diff_state *state)
{
	enum diff_rc rc;
	ARRAYLIST_FREE(state->temp_result);

	if (!algo_config || !algo_config->impl || !state->recursion_depth_left) {
		debug("MAX RECURSION REACHED, just dumping diff chunks\n");
		return diff_algo_none(algo_config, state);
	}

	ARRAYLIST_INIT(state->temp_result, DIFF_RESULT_ALLOC_BLOCKSIZE);
	rc = algo_config->impl(algo_config, state);
	switch (rc) {
	case DIFF_RC_USE_DIFF_ALGO_FALLBACK:
		debug("Got DIFF_RC_USE_DIFF_ALGO_FALLBACK (%p)\n", algo_config->fallback_algo);
		rc = diff_run_algo(algo_config->fallback_algo, state);
		goto return_rc;

	case DIFF_RC_OK:
		/* continue below */
		break;

	default:
		/* some error happened */
		goto return_rc;
	}

	/* Pick up any diff chunks that are still unsolved and feed to inner_algo.
	 * inner_algo will solve unsolved chunks and append to result,
	 * and subsequent solved chunks on this level are then appended to result afterwards. */
	int i;
	for (i = 0; i < state->temp_result.len; i++) {
		struct diff_chunk *c = &state->temp_result.head[i];
		if (c->solved) {
			struct diff_chunk *final_c;
			ARRAYLIST_ADD(final_c, state->result->chunks);
			if (!final_c) {
				rc = DIFF_RC_ENOMEM;
				goto return_rc;
			}
			*final_c = *c;
			continue;
		}

		/* c is an unsolved chunk, feed to inner_algo */
		struct diff_state inner_state = {
			.result = state->result,
			.recursion_depth_left = state->recursion_depth_left - 1,
		};
		diff_data_init_subsection(&inner_state.left, &state->left, c->left_start, c->left_count);
		diff_data_init_subsection(&inner_state.right, &state->right, c->right_start, c->right_count);

		rc = diff_run_algo(algo_config->inner_algo, &inner_state);

		if (rc != DIFF_RC_OK)
			goto return_rc;
	}

	rc = DIFF_RC_OK;
return_rc:
	ARRAYLIST_FREE(state->temp_result);
	return rc;
}

struct diff_result *diff_main(const struct diff_config *config,
			      const uint8_t *left_data, size_t left_len,
			      const uint8_t *right_data, size_t right_len)
{
	struct diff_result *result = malloc(sizeof(struct diff_result));
	if (!result)
		return NULL;

	*result = (struct diff_result){};
	diff_data_init_root(&result->left, left_data, left_len);
	diff_data_init_root(&result->right, right_data, right_len);

	if (!config->atomize_func) {
		result->rc = DIFF_RC_EINVAL;
		return result;
	}

	result->rc = config->atomize_func(config->atomize_func_data, &result->left, &result->right);
	if (result->rc != DIFF_RC_OK)
		return result;

	struct diff_state state = {
		.result = result,
		.recursion_depth_left = config->max_recursion_depth ? : 1024,
	};
	diff_data_init_subsection(&state.left, &result->left, result->left.atoms.head, result->left.atoms.len);
	diff_data_init_subsection(&state.right, &result->right, result->right.atoms.head, result->right.atoms.len);

	result->rc = diff_run_algo(config->algo, &state);

	return result;
}

void diff_result_free(struct diff_result *result)
{
	if (!result)
		return;
	diff_data_free(&result->left);
	diff_data_free(&result->right);
	ARRAYLIST_FREE(result->chunks);
	free(result);
}
