/* Output all lines of a diff_result. */
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

#include <diff/diff_output.h>

enum diff_rc diff_output_plain(FILE *dest, const struct diff_input_info *info,
			       const struct diff_result *result)
{
	if (!result)
		return DIFF_RC_EINVAL;
	if (result->rc != DIFF_RC_OK)
		return result->rc;

	diff_output_info(dest, info);

	int i;
	for (i = 0; i < result->chunks.len; i++) {
		struct diff_chunk *c = &result->chunks.head[i];
		if (c->left_count && c->right_count)
			diff_output_lines(dest, c->solved ? " " : "?", c->left_start, c->left_count);
		else if (c->left_count && !c->right_count)
			diff_output_lines(dest, c->solved ? "-" : "?", c->left_start, c->left_count);
		else if (c->right_count && !c->left_count)
			diff_output_lines(dest, c->solved ? "+" : "?", c->right_start, c->right_count);
	}
	return DIFF_RC_OK;
}

enum diff_rc diff_plain(FILE *dest, const struct diff_config *diff_config,
			const struct diff_input_info *info,
			const char *left, int left_len, const char *right, int right_len)
{
	enum diff_rc rc;
	left_len = left_len < 0 ? strlen(left) : left_len;
	right_len = right_len < 0 ? strlen(right) : right_len;
	struct diff_result *result = diff_main(diff_config, left, left_len, right, right_len);
	rc = diff_output_plain(dest, info, result);
	diff_result_free(result);
	return rc;
}
