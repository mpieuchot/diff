/*	$OpenBSD$	*/

/*
 * Copyright (c) 2018 Martin Pieuchot <mpi@openbsd.org>
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

/* Commandline diff utility to test diff implementations. */

#include <sys/cdefs.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "debug.h"
#include "diff_main.h"

#define DEFAULT_CONTEXT	3

#define F_UNIFIED	(1 << 0)

/* Diff output generators and invocation shims. */
struct diff_input_info {
	const char *left_path;
	const char *left_buffer;
	off_t left_size;
	const char *right_path;
	const char *right_buffer;
	off_t right_size;
};


__dead void	 usage(void);
int		 diffreg(char *, char *, int, int);
char		*mmapfile(const char *, struct stat *);

enum diff_rc diff_plain(FILE *dest, const struct diff_config *diff_config,
			const struct diff_input_info *info);
enum diff_rc diff_unidiff(FILE *dest, const struct diff_config *diff_config,
			  const struct diff_input_info *info,
			  unsigned int context_lines);

const struct diff_algo_config myers, patience, myers_divide;

const struct diff_algo_config myers = {
	.impl = diff_algo_myers,
	.permitted_state_size = 1024 * 1024 * sizeof(int),
	.fallback_algo = &patience,
};

const struct diff_algo_config patience = {
	.impl = diff_algo_patience,
	/* After subdivision, do Patience again. */
	.inner_algo = &patience,
	/* If subdivision failed, do Myers Divide et Impera. */
	.fallback_algo = &myers_divide,
};

const struct diff_algo_config myers_divide = (struct diff_algo_config){
	.impl = diff_algo_myers_divide,
	/* When division succeeded, start from the top. */
	.inner_algo = &myers,
	/* (fallback_algo = NULL implies diff_algo_none). */
	.fallback_algo = NULL,
};

const struct diff_config diff_config = {
	.atomize_func = diff_atomize_text_by_line,
	.algo = &myers,
};

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s file1 file2\n", getprogname());
	exit(1);
}

int
main(int argc, char *argv[])
{
	int ch, context = DEFAULT_CONTEXT, flags = 0;
	long lval;
	char *ep;

	while ((ch = getopt(argc, argv, "uU:")) != -1) {
		switch (ch) {
		case 'U':
			lval = strtol(optarg, &ep, 10);
			if (*ep != '\0' || lval < 0 || lval >= INT_MAX)
				usage();
			context = (int)lval;
			/* FALLTHROUGH */
		case 'u':
			flags |= F_UNIFIED;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 2)
		usage();

	return diffreg(argv[0], argv[1], flags, context);
}

int
diffreg(char *file1, char *file2, int flags, int context)
{
	char *str1, *str2;
	struct stat st1, st2;
	struct diff_input_info info;

	str1 = mmapfile(file1, &st1);
	str2 = mmapfile(file2, &st2);

	info = (struct diff_input_info) {
		.left_path = file1,
		.left_buffer = str1,
		.left_size = st1.st_size,
		.right_path = file2,
		.right_buffer = str2,
		.right_size = st2.st_size,
	};

	if (flags & F_UNIFIED)
		diff_unidiff(stdout, &diff_config, &info, context);
	else
		diff_plain(stdout, &diff_config, &info);

	munmap(str1, st1.st_size);
	munmap(str2, st2.st_size);

	return 0;
}

char *
mmapfile(const char *path, struct stat *st)
{
	int			 fd;
	char			*p;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(2, "%s", path);

	if (fstat(fd, st) == -1)
		err(2, "%s", path);

	if ((uintmax_t)st->st_size > SIZE_MAX)
		errx(2, "%s: file too big to fit memory", path);

	p = mmap(NULL, st->st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED)
		err(2, "mmap");

	close(fd);

	return p;
}

void
output_lines(FILE *dest, const char *prefix, struct diff_atom *start_atom,
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

/*
 * Output all lines of a diff_result.
 */
enum diff_rc
output_plain(FILE *dest, const struct diff_input_info *info,
    const struct diff_result *result)
{
	int i;

	for (i = 0; i < result->chunks.len; i++) {
		struct diff_chunk *c = &result->chunks.head[i];

		if (c->left_count && c->right_count)
			output_lines(dest, c->solved ? " " : "?",
			    c->left_start, c->left_count);
		else if (c->left_count && !c->right_count)
			output_lines(dest, c->solved ? "-" : "?",
			    c->left_start, c->left_count);
		else if (c->right_count && !c->left_count)
			output_lines(dest, c->solved ? "+" : "?",
			    c->right_start, c->right_count);
	}
	return DIFF_RC_OK;
}

enum diff_rc
diff_plain(FILE *dest, const struct diff_config *diff_config,
    const struct diff_input_info *info)
{
	struct diff_result *result;
	int left_len, right_len;
	enum diff_rc rc;

	left_len = info->left_size;
	right_len = info->right_size;
	if (left_len < 0 || right_len < 0)
		return DIFF_RC_EINVAL;
	result = diff_main(diff_config, info->left_buffer, left_len,
	    info->right_buffer, right_len);
	if (result == NULL)
		return DIFF_RC_EINVAL;
	if (result->rc != DIFF_RC_OK)
		return result->rc;
	rc = output_plain(dest, info, result);
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
output_unidiff_chunk(FILE *dest, bool *header_printed,
    const struct diff_input_info *info, const struct diff_result *result,
    const struct chunk_context *cc)
{
	const struct diff_chunk *first_chunk, *last_chunk;
	int chunk_start_line, chunk_end_line, c_idx;

	if (range_empty(&cc->left) && range_empty(&cc->right))
		return;

	if (!(*header_printed)) {
		fprintf(dest, "--- %s\n+++ %s\n",
		    info->left_path ? : "a", info->right_path ? : "b");
		*header_printed = true;
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
		output_lines(dest, " ",
		    &result->left.atoms.head[cc->left.start],
		    chunk_start_line - cc->left.start);

	/* Now write out all the joined chunks and contexts between them */
	for (c_idx = cc->chunk.start; c_idx < cc->chunk.end; c_idx++) {
		const struct diff_chunk *c = &result->chunks.head[c_idx];

		if (c->left_count && c->right_count)
			output_lines(dest, c->solved ? " " : "?",
			    c->left_start, c->left_count);
		else if (c->left_count && !c->right_count)
			output_lines(dest, c->solved ? "-" : "?",
			    c->left_start, c->left_count);
		else if (c->right_count && !c->left_count)
			output_lines(dest, c->solved ? "+" : "?",
			    c->right_start, c->right_count);
	}

	/* Trailing context? */
	last_chunk = &result->chunks.head[cc->chunk.end - 1];
	chunk_end_line = diff_atom_root_idx(&result->left,
	    last_chunk->left_start + last_chunk->left_count);
	if (cc->left.end > chunk_end_line)
		output_lines(dest, " ",
		    &result->left.atoms.head[chunk_end_line],
		    cc->left.end - chunk_end_line);
}

enum diff_rc
output_unidiff(FILE *dest, const struct diff_input_info *info,
    const struct diff_result *result, unsigned int context_lines)
{
	struct chunk_context cc = {};
	bool header_printed = false;
	int i;

	for (i = 0; i < result->chunks.len; i++) {
		struct diff_chunk *c = &result->chunks.head[i];
		enum chunk_type t = chunk_type(c);
		struct chunk_context next;

		if (t != CHUNK_MINUS && t != CHUNK_PLUS)
			continue;

		if (chunk_context_empty(&cc)) {
			/*
			 * These are the first lines being printed.
			 * Note down the start point, any number of
			 * subsequent chunks may be joined up to this
			 * unidiff chunk by context lines or by being
			 * directly adjacent.
			 */
			chunk_context_get(&cc, result, i, context_lines);
			debug("new chunk to be printed:"
			    " chunk %d-%d left %d-%d right %d-%d\n",
			    cc.chunk.start, cc.chunk.end,
			    cc.left.start, cc.left.end, cc.right.start,
			    cc.right.end);
			continue;
		}

		/*
		 * There already is a previous chunk noted down for
		 * being printed.  Does it join up with this one?
		 */
		chunk_context_get(&next, result, i, context_lines);
		debug("new chunk to be printed: chunk %d-%d left %d-%d right"
		    " %d-%d\n", next.chunk.start, next.chunk.end,
		    next.left.start, next.left.end, next.right.start,
		    next.right.end);

		if (chunk_contexts_touch(&cc, &next)) {
			/*
			 * This next context touches or overlaps the
			 * previous one, join.
			 */
			chunk_contexts_merge(&cc, &next);
			debug("new chunk to be printed touches previous chunk,"
			    " now: left %d-%d right %d-%d\n", cc.left.start,
			    cc.left.end, cc.right.start, cc.right.end);
			continue;
		}

		/*
		 * No touching, so the previous context is complete with a
		 * gap between it and this next one.   Print the previous
		 * one and start fresh here.
		 */
		debug("new chunk to be printed does not touch previous chunk;"
		    " print left %d-%d right %d-%d\n", cc.left.start,
		    cc.left.end, cc.right.start, cc.right.end);
		output_unidiff_chunk(dest, &header_printed, info, result,
		    &cc);

		cc = next;
		debug("new unprinted chunk is left %d-%d right %d-%d\n",
		    cc.left.start, cc.left.end, cc.right.start, cc.right.end);

	}

	if (!chunk_context_empty(&cc))
		output_unidiff_chunk(dest, &header_printed, info, result,
		    &cc);
	return DIFF_RC_OK;
}

enum diff_rc
diff_unidiff(FILE *dest, const struct diff_config *diff_config,
    const struct diff_input_info *info, unsigned int context_lines)
{
	struct diff_result *result;
	int left_len, right_len;
	enum diff_rc rc;

	left_len = info->left_size;
	right_len = info->right_size;
	if (left_len < 0 || right_len < 0)
		return DIFF_RC_EINVAL;
	result = diff_main(diff_config, info->left_buffer, left_len,
	    info->right_buffer, right_len);
	if (result == NULL)
		return DIFF_RC_EINVAL;
	if (result->rc != DIFF_RC_OK)
		return result->rc;
	rc = output_unidiff(dest, info, result, context_lines);
	diff_result_free(result);
	return rc;
}
