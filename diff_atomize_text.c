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

/* Split source by line breaks, and calculate a simplistic checksum. */

#include "diff_main.h"

static int
diff_data_atomize_text_lines(struct diff_data *d)
{
	const uint8_t *pos = d->data;
	const uint8_t *end = pos + d->len;
	unsigned int array_size_estimate = d->len / 50;
	unsigned int pow2 = 1;
	enum diff_rc rc;

	while (array_size_estimate >>= 1)
		pow2++;

	ARRAYLIST_INIT(d->atoms, 1 << pow2);

	while (pos < end) {
		const uint8_t *line_end = pos;
		unsigned int hash = 0;
		struct diff_atom *atom;

		while (line_end < end &&
		    *line_end != '\r' && *line_end != '\n') {
			hash = hash * 23 + *line_end;
			line_end++;
		}

		/*
		 * When not at the end of data, the line ending char ('\r'
		 * or '\n') must follow
		 */
		if (line_end < end)
			line_end++;
		/* If that was an '\r', also pull in any following '\n' */
		if (line_end[0] == '\r' && line_end < end && line_end[1] == '\n')
			line_end++;

		/* Record the found line as diff atom */
		ARRAYLIST_ADD(atom, d->atoms);
		if (!atom)
			return DIFF_RC_ENOMEM;

		*atom = (struct diff_atom){
			.at = pos,
			.len = line_end - pos,
			.hash = hash,
		};

		/* Starting point for next line: */
		pos = line_end;
	}

	return DIFF_RC_OK;
}

enum diff_rc
diff_atomize_text_by_line(void *func_data, struct diff_data *left,
    struct diff_data *right)
{
	enum diff_rc rc;

	rc = diff_data_atomize_text_lines(left);
	if (rc != DIFF_RC_OK)
		return rc;
	return diff_data_atomize_text_lines(right);
}
