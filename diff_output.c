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

/* Common parts for printing diff output */

#include "diff_output.h"

void diff_output_lines(FILE *dest, const char *prefix, struct diff_atom *start_atom, unsigned int count)
{
	struct diff_atom *atom;
	foreach_diff_atom(atom, start_atom, count) {
		fprintf(dest, "%s", prefix);
		int i;
		unsigned int len = atom->len;
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

enum diff_rc diff_output_info(FILE *dest, const struct diff_input_info *info)
{
	if (info->arbitrary_info && *info->arbitrary_info)
		fprintf(dest, "%s", info->arbitrary_info);
	fprintf(dest, "--- %s\n+++ %s\n",
		info->left_path ? : "a",
		info->right_path ? : "b");
        return DIFF_RC_OK;
}
