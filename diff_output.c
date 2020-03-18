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

#include "diff_main.h"
#include "debug.h"

/*
 * Common parts for printing diff output
 */

void
diff_output_lines(FILE *dest, const char *prefix, struct diff_atom *start_atom,
    unsigned int count)
{
	struct diff_atom *atom;

	foreach_diff_atom(atom, start_atom, count) {
		unsigned int len = atom->len;
		int i;

		fprintf(dest, "%s", prefix);
		if (len && atom->at[len - 1] == '\n') {
			len--;
			if (len && atom->at[len - 1] == '\r')
				len--;
		}

		for (i = 0; i < len; i++) {
			char c = atom->at[i];
			if ((c < 0x20 || c >= 0x7f) && c != '\t')
				fprintf(dest, "\\x%02x", (unsigned char)c);
			else
				fprintf(dest, "%c", c);
		}
		fprintf(dest, "\n");
	}
}

enum diff_rc
diff_output_info(FILE *dest, const struct diff_input_info *info)
{
	if (info->arbitrary_info && *info->arbitrary_info)
		fprintf(dest, "%s", info->arbitrary_info);
	fprintf(dest, "--- %s\n+++ %s\n",
	    info->left_path ? : "a",
	    info->right_path ? : "b");
	return DIFF_RC_OK;
}

/*
 * Output all lines of a diff_result.
 */
enum diff_rc
diff_output_plain(FILE *dest, const struct diff_input_info *info,
    const struct diff_result *result)
{
	int i;

	if (!result)
		return DIFF_RC_EINVAL;
	if (result->rc != DIFF_RC_OK)
		return result->rc;

	diff_output_info(dest, info);

	for (i = 0; i < result->chunks.len; i++) {
		struct diff_chunk *c = &result->chunks.head[i];

		if (c->left_count && c->right_count)
			diff_output_lines(dest, c->solved ? " " : "?",
			    c->left_start, c->left_count);
		else if (c->left_count && !c->right_count)
			diff_output_lines(dest, c->solved ? "-" : "?",
			    c->left_start, c->left_count);
		else if (c->right_count && !c->left_count)
			diff_output_lines(dest, c->solved ? "+" : "?",
			    c->right_start, c->right_count);
	}
	return DIFF_RC_OK;
}

enum diff_rc
diff_plain(FILE *dest, const struct diff_config *diff_config,
    const struct diff_input_info *info,
    const char *left, int left_len, const char *right, int right_len)
{
	struct diff_result *result;
	enum diff_rc rc;

	left_len = left_len < 0 ? strlen(left) : left_len;
	right_len = right_len < 0 ? strlen(right) : right_len;
	result = diff_main(diff_config, left, left_len, right, right_len);
	rc = diff_output_plain(dest, info, result);
	diff_result_free(result);
	return rc;
}

/*
 * Produce a unidiff output from a diff_result.
 */
enum chunk_type {
	CHUNK_EMPTY,
	    CHUNK_PLUS,
	    CHUNK_MINUS,
	    CHUNK_SAME,
	    CHUNK_WEIRD,
};

static inline enum chunk_type
chunk_type(const struct diff_chunk *chunk)
{
	if (!chunk->left_count && !chunk->right_count)
		return CHUNK_EMPTY;
	if (!chunk->solved)
		return CHUNK_WEIRD;
	if (!chunk->right_count)
		return CHUNK_MINUS;
	if (!chunk->left_count)
		return CHUNK_PLUS;
	if (chunk->left_count != chunk->right_count)
		return CHUNK_WEIRD;
	return CHUNK_SAME;
}

struct chunk_context {
	struct range chunk;
	struct range left, right;
};

static bool
chunk_context_empty(const struct chunk_context *cc)
{
	return range_empty(&cc->chunk);
}

static void
chunk_context_get(struct chunk_context *cc, const struct diff_result *r,
    int chunk_idx, int context_lines)
{
	const struct diff_chunk *c = &r->chunks.head[chunk_idx];
	int left_start, right_start;

	left_start = diff_atom_root_idx(&r->left, c->left_start);
	right_start = diff_atom_root_idx(&r->right, c->right_start);

	*cc = (struct chunk_context) {
		.chunk = {
			.start = chunk_idx,
			.end = chunk_idx + 1,
		},
		.left = {
			.start = MAX(0, left_start - context_lines),
			.end = MIN(r->left.atoms.len,
			    left_start + c->left_count + context_lines),
		},
		.right = {
			.start = MAX(0, right_start - context_lines),
			.end = MIN(r->right.atoms.len,
			    right_start + c->right_count + context_lines),
		},
	};
}

static bool
chunk_contexts_touch(const struct chunk_context *cc, const struct chunk_context *other)
{
	return ranges_touch(&cc->chunk, &other->chunk) ||
	    ranges_touch(&cc->left, &other->left) ||
	    ranges_touch(&cc->right, &other->right);
}

static void
chunk_contexts_merge(struct chunk_context *cc, const struct chunk_context *other)
{
	ranges_merge(&cc->chunk, &other->chunk);
	ranges_merge(&cc->left, &other->left);
	ranges_merge(&cc->right, &other->right);
}

static void
diff_output_unidiff_chunk(FILE *dest, bool *info_printed,
    const struct diff_input_info *info, const struct diff_result *result,
    const struct chunk_context *cc)
{
	const struct diff_chunk *first_chunk, *last_chunk;
	int chunk_start_line, chunk_end_line, c_idx;

	if (range_empty(&cc->left) && range_empty(&cc->right))
		return;

	if (!(*info_printed)) {
		diff_output_info(dest, info);
		*info_printed = true;
	}

	fprintf(dest, "@@ -%d,%d +%d,%d @@\n",
	    cc->left.start + 1, cc->left.end - cc->left.start,
	    cc->right.start + 1, cc->right.end - cc->right.start);

	/*
	 * Got the absolute line numbers where to start printing, and the
	 * index of the interesting (non-context) chunk.
	 * To print context lines above the interesting chunk, nipping on
	 * the previous chunk index may be necessary.
	 * It is guaranteed to be only context lines where left == right,
	 * so it suffices to look on the left.
	 */
	first_chunk = &result->chunks.head[cc->chunk.start];
	chunk_start_line = diff_atom_root_idx(&result->left,
	    first_chunk->left_start);

	if (cc->left.start < chunk_start_line)
		diff_output_lines(dest, " ",
		    &result->left.atoms.head[cc->left.start],
		    chunk_start_line - cc->left.start);

	/* Now write out all the joined chunks and contexts between them */
	for (c_idx = cc->chunk.start; c_idx < cc->chunk.end; c_idx++) {
		const struct diff_chunk *c = &result->chunks.head[c_idx];

		if (c->left_count && c->right_count)
			diff_output_lines(dest, c->solved ? " " : "?",
			    c->left_start, c->left_count);
		else if (c->left_count && !c->right_count)
			diff_output_lines(dest, c->solved ? "-" : "?",
			    c->left_start, c->left_count);
		else if (c->right_count && !c->left_count)
			diff_output_lines(dest, c->solved ? "+" : "?",
			    c->right_start, c->right_count);
	}

	/* Trailing context? */
	last_chunk = &result->chunks.head[cc->chunk.end - 1];
	chunk_end_line = diff_atom_root_idx(&result->left,
	    last_chunk->left_start + last_chunk->left_count);
	if (cc->left.end > chunk_end_line)
		diff_output_lines(dest, " ",
		    &result->left.atoms.head[chunk_end_line],
		    cc->left.end - chunk_end_line);
}

enum diff_rc
diff_output_unidiff(FILE *dest, const struct diff_input_info *info,
    const struct diff_result *result, unsigned int context_lines)
{
	struct chunk_context cc = {};
	bool info_printed = false;
	int i;

	if (!result)
		return DIFF_RC_EINVAL;
	if (result->rc != DIFF_RC_OK)
		return result->rc;

	for (i = 0; i < result->chunks.len; i++) {
		struct diff_chunk *c = &result->chunks.head[i];
		enum chunk_type t = chunk_type(c);

		if (t == CHUNK_MINUS || t == CHUNK_PLUS) {
			if (chunk_context_empty(&cc)) {
				/*
				 * These are the first lines being printed.
				 * Note down the start point, any number of
				 * subsequent chunks may be joined up to this
				 * unidiff chunk by context lines or by being
				 * directly adjacent.
				 */
				chunk_context_get(&cc, result, i,
				    context_lines);
				debug("new chunk to be printed:"
				    " chunk %d-%d left %d-%d right %d-%d\n",
				    cc.chunk.start, cc.chunk.end,
				    cc.left.start, cc.left.end, cc.right.start,
				    cc.right.end);
			} else {
				struct chunk_context next;

				/*
				 * There already is a previous chunk noted down
				 * for being printed.
				 * Does it join up with this one?
				 */
				chunk_context_get(&next, result, i,
				    context_lines);
				debug("new chunk to be printed:"
				    " chunk %d-%d left %d-%d right %d-%d\n",
				    next.chunk.start, next.chunk.end,
				    next.left.start, next.left.end,
				    next.right.start, next.right.end);
				if (chunk_contexts_touch(&cc, &next)) {
					/*
					 * This next context touches or
					 * overlaps the previous one, join.
					 */
					chunk_contexts_merge(&cc, &next);
					debug("new chunk to be printed touches"
					    " previous chunk, now: left %d-%d"
					    " right %d-%d\n",
					    cc.left.start, cc.left.end,
					    cc.right.start, cc.right.end);
				} else {
					/*
					 * No touching, so the previous context
					 * is complete with a gap between it
					 * and this next one.
					 * Print the previous one and start
					 * fresh here.
					 */
					debug("new chunk to be printed does not"
					    "touch previous chunk;"
					    " print left %d-%d right %d-%d\n",
					    cc.left.start, cc.left.end,
					    cc.right.start, cc.right.end);
					diff_output_unidiff_chunk(dest,
					    &info_printed, info, result, &cc);
					cc = next;
					debug("new unprinted chunk is left"
					    " %d-%d right %d-%d\n",
					    cc.left.start, cc.left.end,
					    cc.right.start, cc.right.end);
				}
			}
		}

	}

	if (!chunk_context_empty(&cc))
		diff_output_unidiff_chunk(dest, &info_printed, info, result,
		    &cc);
	return DIFF_RC_OK;
}

enum diff_rc
diff_unidiff(FILE *dest, const struct diff_config *diff_config,
    const struct diff_input_info *info,
    const char *left, int left_len, const char *right, int right_len,
    unsigned int context_lines)
{
	struct diff_result *result;
	enum diff_rc rc;

	left_len = left_len < 0 ? strlen(left) : left_len;
	right_len = right_len < 0 ? strlen(right) : right_len;
	result = diff_main(diff_config, left, left_len, right, right_len);
	rc = diff_output_unidiff(dest, info, result, context_lines);
	diff_result_free(result);
	return rc;
}
