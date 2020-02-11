/* Diff output generators and invocation shims. */
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

#pragma once

#include <stdio.h>
#include "diff_main.h"

struct diff_input_info {
	const char *arbitrary_info;
	const char *left_path;
	const char *right_path;
};

enum diff_rc diff_output_plain(FILE *dest, const struct diff_input_info *info,
			       const struct diff_result *result);
enum diff_rc diff_plain(FILE *dest, const struct diff_config *diff_config,
			const struct diff_input_info *info,
			const char *left, int left_len, const char *right, int right_len);

enum diff_rc diff_output_unidiff(FILE *dest, const struct diff_input_info *info,
				 const struct diff_result *result, unsigned int context_lines);
enum diff_rc diff_unidiff(FILE *dest, const struct diff_config *diff_config,
			  const struct diff_input_info *info,
			  const char *left, int left_len, const char *right, int right_len,
			  unsigned int context_lines);

enum diff_rc diff_output_info(FILE *dest, const struct diff_input_info *info);
void diff_output_lines(FILE *dest, const char *prefix, struct diff_atom *start_atom, unsigned int count);
