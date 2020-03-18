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

#pragma once

#define DEBUG 0

#if DEBUG
#include <stdio.h>
#define print(args...) fprintf(stderr, ##args)
#define debug print
#define debug_dump dump
#define debug_dump_atom dump_atom
#define debug_dump_atoms dump_atoms

static inline void
dump_atom(const struct diff_data *left, const struct diff_data *right, const struct diff_atom *atom)
{
	const char *s;

	if (!atom) {
		print("NULL atom\n");
		return;
	}
	if (left)
		print(" %3ld", diff_atom_root_idx(left, atom));
	if (right && atom->patience.pos_in_other)
		print(" %3ld", diff_atom_root_idx(right,
		    atom->patience.pos_in_other));

	print(" %s%s '", atom->patience.unique_here ? "u" : " ",
	    atom->patience.unique_in_both ? "c" : " ");
	for (s = atom->at; s < (const char*)(atom->at + atom->len); s++) {
		if (*s == '\r')
			print("\\r");
		else if (*s == '\n')
			print("\\n");
		else if ((*s < 32 || *s >= 127) && (*s != '\t'))
			print("\\x%02x", *s);
		else
			print("%c", *s);
	}
	print("'\n");
}

static inline void
dump_atoms(const struct diff_data *d, struct diff_atom *atom, unsigned int count)
{
	if (count > 42) {
		dump_atoms(d, atom, 20);
		print("[%u lines skipped]\n", count - 20 - 20);
		dump_atoms(d, atom + count - 20, 20);
		return;
	} else {
		struct diff_atom *i;
		foreach_diff_atom(i, atom, count) {
			dump_atom(d, NULL, i);
		}
	}
}

static inline void
dump(struct diff_data *d)
{
	dump_atoms(d, d->atoms.head, d->atoms.len);
}

static inline void
dump_myers_graph(const struct diff_data *l, const struct diff_data *r, int *kd)
{
	int x, y;

	print("  ");
	for (x = 0; x <= l->atoms.len; x++)
		print("%2d", x);
	print("\n");

	for (y = 0; y <= r->atoms.len; y++) {
		print("%2d ", y);
		for (x = 0; x <= l->atoms.len; x++) {

			/* print d advancements from kd, if any. */
			int d = -1;
			if (kd) {
				int max = l->atoms.len + r->atoms.len;
				size_t kd_len = max + 1 + max;
				int *kd_pos = kd;
				int di;
#define xk_to_y(X, K) ((X) - (K))
				for (di = 0; di < max; di++) {
					int ki;
					for (ki = di; ki >= -di; ki -= 2) {
						if (x == kd_pos[ki]
						    && y == xk_to_y(x, ki)) {
							d = di;
							break;
						}
					}
					if (d >= 0)
						break;
					kd_pos += kd_len;
				}
			}
			if (d >= 0)
				print("%d", d);
			else
				print("o");
			if (x < l->atoms.len && d < 10)
				print("-");
		}
		print("\n");
		if (y == r->atoms.len)
			break;

		print("   ");
		for (x = 0; x < l->atoms.len; x++) {
			if (diff_atom_same(&l->atoms.head[x], &r->atoms.head[y]))
				print("|\\");
			else
				print("| ");
		}
		print("|\n");
	}
}

static inline void
debug_dump_myers_graph(const struct diff_data *l, const struct diff_data *r,
    int *kd)
{
	if (l->atoms.len > 99 || r->atoms.len > 99)
		return;
	dump_myers_graph(l, r, kd);
}

#else
#define debug(args...)
#define debug_dump(args...)
#define debug_dump_atom(args...)
#define debug_dump_atoms(args...)
#define debug_dump_myers_graph(args...)
#endif
