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

/*
 * Myers diff algorithm implementation, invented by Eugene W. Myers [1].
 * Implementations of both the Myers Divide Et Impera (using linear space)
 * and the canonical Myers algorithm (using quadratic space).
 */

#include "diff_main.h"
#include "debug.h"

/* Myers' diff algorithm [1] is nicely explained in [2].
 * [1] http://www.xmailserver.org/diff2.pdf
 * [2] https://blog.jcoglan.com/2017/02/12/the-myers-diff-algorithm-part-1/ ff.
 *
 * Myers approaches finding the smallest diff as a graph problem.
 * The crux is that the original algorithm requires quadratic amount of memory:
 * both sides' lengths added, and that squared. So if we're diffing lines of text, two files with 1000 lines each would
 * blow up to a matrix of about 2000 * 2000 ints of state, about 16 Mb of RAM to figure out 2 kb of text.
 * The solution is using Myers' "divide and conquer" extension algorithm, which does the original traversal from both
 * ends of the files to reach a middle where these "snakes" touch, hence does not need to backtrace the traversal, and
 * so gets away with only keeping a single column of that huge state matrix in memory.
 *
 * Todo: the divide and conquer requires linear *space*, not necessarily linear *time*. It recurses, apparently doing
 * multiple Myers passes, and also it apparently favors fragmented diffs in cases where chunks of text were moved to a
 * different place. Up to a given count of diff atoms (text lines), it might be desirable to accept the quadratic memory
 * usage, get nicer diffs and less re-iteration of the same data?
 */

struct diff_box {
	unsigned int left_start;
	unsigned int left_end;
	unsigned int right_start;
	unsigned int right_end;
};

#define diff_box_empty(DIFF_SNAKE) ((DIFF_SNAKE)->left_end == 0)


/* If the two contents of a file are A B C D E and X B C Y,
 * the Myers diff graph looks like:
 *
 *   k0  k1
 *    \   \
 * k-1     0 1 2 3 4 5
 *   \      A B C D E
 *     0   o-o-o-o-o-o
 *      X  | | | | | |
 *     1   o-o-o-o-o-o
 *      B  | |\| | | |
 *     2   o-o-o-o-o-o
 *      C  | | |\| | |
 *     3   o-o-o-o-o-o
 *      Y  | | | | | |\
 *     4   o-o-o-o-o-o c1
 *                  \ \
 *                 c-1 c0
 *
 * Moving right means delete an atom from the left-hand-side,
 * Moving down means add an atom from the right-hand-side.
 * Diagonals indicate identical atoms on both sides, the challenge is to use as many diagonals as possible.
 *
 * The original Myers algorithm walks all the way from the top left to the bottom right, remembers all steps, and then
 * backtraces to find the shortest path. However, that requires keeping the entire graph in memory, which needs
 * quadratic space.
 *
 * Myers adds a variant that uses linear space -- note, not linear time, only linear space: walk forward and backward,
 * find a meeting point in the middle, and recurse on the two separate sections. This is called "divide and conquer".
 *
 * d: the step number, starting with 0, a.k.a. the distance from the starting point.
 * k: relative index in the state array for the forward scan, indicating on which diagonal through the diff graph we
 *    currently are.
 * c: relative index in the state array for the backward scan, indicating the diagonal number from the bottom up.
 *
 * The "divide and conquer" traversal through the Myers graph looks like this:
 *
 *      | d=   0   1   2   3      2   1   0
 *  ----+--------------------------------------------
 *  k=  |                                      c=
 *   4  |                                       3
 *      |
 *   3  |                 3,0    5,2            2
 *      |                /          \
 *   2  |             2,0            5,3        1
 *      |            /                 \
 *   1  |         1,0     4,3 >= 4,3    5,4<--  0
 *      |        /       /          \  /
 *   0  |  -->0,0     3,3            4,4       -1
 *      |        \   /              /
 *  -1  |         0,1     1,2    3,4           -2
 *      |            \   /
 *  -2  |             0,2                      -3
 *      |                \
 *      |                 0,3
 *      |  forward->                 <-backward
 *
 * x,y pairs here are the coordinates in the Myers graph:
 * x = atom index in left-side source, y = atom index in the right-side source.
 *
 * Only one forward column and one backward column are kept in mem, each need at most left.len + 1 + right.len items.
 * Note that each d step occupies either the even or the odd items of a column: if e.g. the previous column is in the
 * odd items, the next column is formed in the even items, without overwriting the previous column's results.
 *
 * Also note that from the diagonal index k and the x coordinate, the y coordinate can be derived:
 *    y = x - k
 * Hence the state array only needs to keep the x coordinate, i.e. the position in the left-hand file, and the y
 * coordinate, i.e. position in the right-hand file, is derived from the index in the state array.
 *
 * The two traces meet at 4,3, the first step (here found in the forward traversal) where a forward position is on or
 * past a backward traced position on the same diagonal.
 *
 * This divides the problem space into:
 *
 *         0 1 2 3 4 5
 *          A B C D E
 *     0   o-o-o-o-o
 *      X  | | | | |
 *     1   o-o-o-o-o
 *      B  | |\| | |
 *     2   o-o-o-o-o
 *      C  | | |\| |
 *     3   o-o-o-o-*-o   *: forward and backward meet here
 *      Y          | |
 *     4           o-o
 *
 * Doing the same on each section lead to:
 *
 *         0 1 2 3 4 5
 *          A B C D E
 *     0   o-o
 *      X  | |
 *     1   o-b    b: backward d=1 first reaches here (sliding up the snake)
 *      B     \   f: then forward d=2 reaches here (sliding down the snake)
 *     2       o     As result, the box from b to f is found to be identical;
 *      C       \    leaving a top box from 0,0 to 1,1 and a bottom trivial tail 3,3 to 4,3.
 *     3         f-o
 *
 *     3           o-*
 *      Y            |
 *     4             o   *: forward and backward meet here
 *
 * and solving the last top left box gives:
 *
 *         0 1 2 3 4 5
 *          A B C D E           -A
 *     0   o-o                  +X
 *      X    |                   B
 *     1     o                   C
 *      B     \                 -D
 *     2       o                -E
 *      C       \               +Y
 *     3         o-o-o
 *      Y            |
 *     4             o
 *
 */

#define xk_to_y(X, K) ((X) - (K))
#define xc_to_y(X, C, DELTA) ((X) - (C) + (DELTA))
#define k_to_c(K, DELTA) ((K) + (DELTA))
#define c_to_k(C, DELTA) ((C) - (DELTA))

/* Do one forwards step in the "divide and conquer" graph traversal.
 * left: the left side to diff.
 * right: the right side to diff against.
 * kd_forward: the traversal state for forwards traversal, modified by this function.
 *             This is carried over between invocations with increasing d.
 *             kd_forward points at the center of the state array, allowing negative indexes.
 * kd_backward: the traversal state for backwards traversal, to find a meeting point.
 *              Since forwards is done first, kd_backward will be valid for d - 1, not d.
 *              kd_backward points at the center of the state array, allowing negative indexes.
 * d: Step or distance counter, indicating for what value of d the kd_forward should be populated.
 *    For d == 0, kd_forward[0] is initialized, i.e. the first invocation should be for d == 0.
 * meeting_snake: resulting meeting point, if any.
 */
static void diff_divide_myers_forward(struct diff_data *left, struct diff_data *right,
				      int *kd_forward, int *kd_backward, int d,
				      struct diff_box *meeting_snake)
{
	int delta = (int)right->atoms.len - (int)left->atoms.len;
	int prev_x;
	int prev_y;
	int k;
	int x;

	debug("-- %s d=%d\n", __func__, d);
	debug_dump_myers_graph(left, right, NULL);

	for (k = d; k >= -d; k -= 2) {
		if (k < -(int)right->atoms.len || k > (int)left->atoms.len) {
			/* This diagonal is completely outside of the Myers graph, don't calculate it. */
			if (k < -(int)right->atoms.len)
				debug(" %d k < -(int)right->atoms.len %d\n", k, -(int)right->atoms.len);
			else
				debug(" %d k > left->atoms.len %d\n", k, left->atoms.len);
			if (k < 0) {
				/* We are traversing negatively, and already below the entire graph, nothing will come
				 * of this. */
				debug(" break");
				break;
			}
			debug(" continue");
			continue;
		}
		debug("- k = %d\n", k);
		if (d == 0) {
			/* This is the initializing step. There is no prev_k yet, get the initial x from the top left of
			 * the Myers graph. */
			x = 0;
		}
		/* Favoring "-" lines first means favoring moving rightwards in the Myers graph.
		 * For this, all k should derive from k - 1, only the bottom most k derive from k + 1:
		 *
		 *      | d=   0   1   2
		 *  ----+----------------
		 *  k=  |
		 *   2  |             2,0 <-- from prev_k = 2 - 1 = 1
		 *      |            /
		 *   1  |         1,0
		 *      |        /
		 *   0  |  -->0,0     3,3
		 *      |       \\   /
		 *  -1  |         0,1 <-- bottom most for d=1 from prev_k = -1 + 1 = 0
		 *      |           \\
		 *  -2  |             0,2 <-- bottom most for d=2 from prev_k = -2 + 1 = -1
		 *
		 * Except when a k + 1 from a previous run already means a further advancement in the graph.
		 * If k == d, there is no k + 1 and k - 1 is the only option.
		 * If k < d, use k + 1 in case that yields a larger x. Also use k + 1 if k - 1 is outside the graph.
		 */
		else if (k > -d && (k == d
				    || (k - 1 >= -(int)right->atoms.len
					&& kd_forward[k - 1] >= kd_forward[k + 1]))) {
			/* Advance from k - 1.
			 * From position prev_k, step to the right in the Myers graph: x += 1.
			 */
			int prev_k = k - 1;
			prev_x = kd_forward[prev_k];
			prev_y = xk_to_y(prev_x, prev_k);
			x = prev_x + 1;
		} else {
			/* The bottom most one.
			 * From position prev_k, step to the bottom in the Myers graph: y += 1.
			 * Incrementing y is achieved by decrementing k while keeping the same x.
			 * (since we're deriving y from y = x - k).
			 */
			int prev_k = k + 1;
			prev_x = kd_forward[prev_k];
			prev_y = xk_to_y(prev_x, prev_k);
			x = prev_x;
		}

		/* Slide down any snake that we might find here. */
		while (x < left->atoms.len && xk_to_y(x, k) < right->atoms.len
		       && diff_atom_same(&left->atoms.head[x], &right->atoms.head[xk_to_y(x, k)]))
		       x++;
		kd_forward[k] = x;

		if (DEBUG) {
			int fi;
			for (fi = d; fi >= k; fi--) {
				debug("kd_forward[%d] = (%d, %d)\n", fi, kd_forward[fi], kd_forward[fi] - fi);
				/*
				if (kd_forward[fi] >= 0 && kd_forward[fi] < left->atoms.len)
					debug_dump_atom(left, right, &left->atoms.head[kd_forward[fi]]);
				else
					debug("\n");
				if (kd_forward[fi]-fi >= 0 && kd_forward[fi]-fi < right->atoms.len)
					debug_dump_atom(right, left, &right->atoms.head[kd_forward[fi]-fi]);
				else
					debug("\n");
				*/
			}
		}

		if (x < 0 || x > left->atoms.len
		    || xk_to_y(x, k) < 0 || xk_to_y(x, k) > right->atoms.len)
			continue;

		/* Figured out a new forwards traversal, see if this has gone onto or even past a preceding backwards
		 * traversal.
		 *
		 * If the delta in length is odd, then d and backwards_d hit the same state indexes:
		 *      | d=   0   1   2      1   0
		 *  ----+----------------    ----------------
		 *  k=  |                              c=
		 *   4  |                               3
		 *      |
		 *   3  |                               2
		 *      |                same
		 *   2  |             2,0====5,3        1
		 *      |            /          \
		 *   1  |         1,0            5,4<-- 0
		 *      |        /              /
		 *   0  |  -->0,0     3,3====4,4       -1
		 *      |        \   /
		 *  -1  |         0,1                  -2
		 *      |            \
		 *  -2  |             0,2              -3
		 *      |
		 *
		 * If the delta is even, they end up off-by-one, i.e. on different diagonals:
		 *
		 *      | d=   0   1   2    1   0
		 *  ----+----------------  ----------------
		 *      |                            c=
		 *   3  |                             3
		 *      |
		 *   2  |             2,0 off         2
		 *      |            /   \\
		 *   1  |         1,0      4,3        1
		 *      |        /       //   \
		 *   0  |  -->0,0     3,3      4,4<-- 0
		 *      |        \   /        /
		 *  -1  |         0,1      3,4       -1
		 *      |            \   //
		 *  -2  |             0,2            -2
		 *      |
		 *
		 * So in the forward path, we can only match up diagonals when the delta is odd.
		 */
		 /* Forwards is done first, so the backwards one was still at d - 1. Can't do this for d == 0. */
		int backwards_d = d - 1;
		if ((delta & 1) && (backwards_d >= 0)) {
			debug("backwards_d = %d\n", backwards_d);

			/* If both sides have the same length, forward and backward start on the same diagonal, meaning the
			 * backwards state index c == k.
			 * As soon as the lengths are not the same, the backwards traversal starts on a different diagonal, and
			 * c = k shifted by the difference in length.
			 */
			int c = k_to_c(k, delta);

			/* When the file sizes are very different, the traversal trees start on far distant diagonals.
			 * They don't necessarily meet straight on. See whether this forward value is on a diagonal that
			 * is also valid in kd_backward[], and match them if so. */
			if (c >= -backwards_d && c <= backwards_d) {
				/* Current k is on a diagonal that exists in kd_backward[]. If the two x positions have
				 * met or passed (forward walked onto or past backward), then we've found a midpoint / a
				 * mid-box.
				 *
				 * But we need to avoid matching a situation like this:
				 *       0  1
				 *        x y
				 *   0   o-o-o
				 *     x |\| |
				 *   1   o-o-o
				 *     y | |\|
				 *   2  (B)o-o  <--(B) backwards traversal reached here
				 *     a | | |
				 *   3   o-o-o<-- prev_x, prev_y
				 *     b | | |
				 *   4   o-o(F) <--(F) forwards traversal  reached here
				 *     x |\| |     Now both are on the same diagonal and look like they passed,
				 *   5   o-o-o     but actually they have sneaked past each other and have not met.
				 *     y | |\|
				 *   6   o-o-o
				 *
				 * The solution is to notice that prev_x,prev_y were also already past (B).
				 */
				int backward_x = kd_backward[c];
				int backward_y = xc_to_y(backward_x, c, delta);
				debug(" prev_x,y = (%d,%d)  c%d:backward_x,y = (%d,%d)  k%d:x,y = (%d,%d)\n",
				      prev_x, prev_y, c, backward_x, backward_y, k, x, xk_to_y(x, k));
				if (prev_x <= backward_x && prev_y <= backward_y
				    && x >= backward_x) {
					*meeting_snake = (struct diff_box){
						.left_start = backward_x,
						.left_end = x,
						.right_start = xc_to_y(backward_x, c, delta),
						.right_end = xk_to_y(x, k),
					};
					debug("HIT x=(%u,%u) - y=(%u,%u)\n",
					      meeting_snake->left_start,
					      meeting_snake->right_start,
					      meeting_snake->left_end,
					      meeting_snake->right_end);
					return;
				}
			}
		}
	}
}

/* Do one backwards step in the "divide and conquer" graph traversal.
 * left: the left side to diff.
 * right: the right side to diff against.
 * kd_forward: the traversal state for forwards traversal, to find a meeting point.
 *             Since forwards is done first, after this, both kd_forward and kd_backward will be valid for d.
 *             kd_forward points at the center of the state array, allowing negative indexes.
 * kd_backward: the traversal state for backwards traversal, to find a meeting point.
 *              This is carried over between invocations with increasing d.
 *              kd_backward points at the center of the state array, allowing negative indexes.
 * d: Step or distance counter, indicating for what value of d the kd_backward should be populated.
 *    Before the first invocation, kd_backward[0] shall point at the bottom right of the Myers graph
 *    (left.len, right.len).
 *    The first invocation will be for d == 1.
 * meeting_snake: resulting meeting point, if any.
 */
static void diff_divide_myers_backward(struct diff_data *left, struct diff_data *right,
				       int *kd_forward, int *kd_backward, int d,
				       struct diff_box *meeting_snake)
{
	int delta = (int)right->atoms.len - (int)left->atoms.len;
	int prev_x;
	int prev_y;
	int c;
	int x;

	debug("-- %s d=%d\n", __func__, d);
	debug_dump_myers_graph(left, right, NULL);

	for (c = d; c >= -d; c -= 2) {
		if (c < -(int)left->atoms.len || c > (int)right->atoms.len) {
			/* This diagonal is completely outside of the Myers graph, don't calculate it. */
			if (c < -(int)left->atoms.len)
				debug(" %d c < -(int)left->atoms.len %d\n", c, -(int)left->atoms.len);
			else
				debug(" %d c > right->atoms.len %d\n", c, right->atoms.len);
			if (c < 0) {
				/* We are traversing negatively, and already below the entire graph, nothing will come
				 * of this. */
				debug(" break");
				break;
			}
			debug(" continue");
			continue;
		}
		debug("- c = %d\n", c);
		if (d == 0) {
			/* This is the initializing step. There is no prev_c yet, get the initial x from the bottom
			 * right of the Myers graph. */
			x = left->atoms.len;
		}
		/* Favoring "-" lines first means favoring moving rightwards in the Myers graph.
		 * For this, all c should derive from c - 1, only the bottom most c derive from c + 1:
		 *
		 *                                  2   1   0
		 *  ---------------------------------------------------
		 *                                               c=
		 *                                                3
		 *
		 *         from prev_c = c - 1 --> 5,2            2
		 *                                    \
		 *                                     5,3        1
		 *                                        \
		 *                                 4,3     5,4<-- 0
		 *                                    \   /
		 *  bottom most for d=1 from c + 1 --> 4,4       -1
		 *                                    /
		 *         bottom most for d=2 --> 3,4           -2
		 *
		 * Except when a c + 1 from a previous run already means a further advancement in the graph.
		 * If c == d, there is no c + 1 and c - 1 is the only option.
		 * If c < d, use c + 1 in case that yields a larger x. Also use c + 1 if c - 1 is outside the graph.
		 */
		else if (c > -d && (c == d
				    || (c - 1 >= -(int)right->atoms.len
					&& kd_backward[c - 1] <= kd_backward[c + 1]))) {
			/* A top one.
			 * From position prev_c, step upwards in the Myers graph: y -= 1.
			 * Decrementing y is achieved by incrementing c while keeping the same x.
			 * (since we're deriving y from y = x - c + delta).
			 */
			int prev_c = c - 1;
			prev_x = kd_backward[prev_c];
			prev_y = xc_to_y(prev_x, prev_c, delta);
			x = prev_x;
		} else {
			/* The bottom most one.
			 * From position prev_c, step to the left in the Myers graph: x -= 1.
			 */
			int prev_c = c + 1;
			prev_x = kd_backward[prev_c];
			prev_y = xc_to_y(prev_x, prev_c, delta);
			x = prev_x - 1;
		}

		/* Slide up any snake that we might find here. */
		debug("c=%d x-1=%d Yb-1=%d-1=%d\n", c, x-1, xc_to_y(x, c, delta), xc_to_y(x, c, delta)-1);
		if (x > 0) {
			debug("  l="); debug_dump_atom(left, right, &left->atoms.head[x-1]);
		}
		if (xc_to_y(x, c, delta) > 0) {
			debug("   r="); debug_dump_atom(right, left, &right->atoms.head[xc_to_y(x, c, delta)-1]);
		}
		while (x > 0 && xc_to_y(x, c, delta) > 0
		       && diff_atom_same(&left->atoms.head[x-1], &right->atoms.head[xc_to_y(x, c, delta)-1]))
		       x--;
		kd_backward[c] = x;

		if (DEBUG) {
			int fi;
			for (fi = d; fi >= c; fi--) {
				debug("kd_backward[%d] = (%d, %d)\n", fi, kd_backward[fi],
				      kd_backward[fi] - fi + delta);
				/*
				if (kd_backward[fi] >= 0 && kd_backward[fi] < left->atoms.len)
					debug_dump_atom(left, right, &left->atoms.head[kd_backward[fi]]);
				else
					debug("\n");
				if (kd_backward[fi]-fi+delta >= 0 && kd_backward[fi]-fi+delta < right->atoms.len)
					debug_dump_atom(right, left, &right->atoms.head[kd_backward[fi]-fi+delta]);
				else
					debug("\n");
				*/
			}
		}

		if (x < 0 || x > left->atoms.len
		    || xc_to_y(x, c, delta) < 0 || xc_to_y(x, c, delta) > right->atoms.len)
			continue;

		/* Figured out a new backwards traversal, see if this has gone onto or even past a preceding forwards
		 * traversal.
		 *
		 * If the delta in length is even, then d and backwards_d hit the same state indexes -- note how this is
		 * different from in the forwards traversal, because now both d are the same:
		 *
		 *      | d=   0   1   2      2   1   0
		 *  ----+----------------    --------------------
		 *  k=  |                                  c=
		 *   4  |
		 *      |
		 *   3  |                                   3
		 *      |                same
		 *   2  |             2,0====5,2            2
		 *      |            /          \
		 *   1  |         1,0            5,3        1
		 *      |        /              /  \
		 *   0  |  -->0,0     3,3====4,3    5,4<--  0
		 *      |        \   /             /
		 *  -1  |         0,1            4,4       -1
		 *      |            \
		 *  -2  |             0,2                  -2
		 *      |
		 *                                      -3
		 * If the delta is odd, they end up off-by-one, i.e. on different diagonals.
		 * So in the backward path, we can only match up diagonals when the delta is even.
		 */
		if ((delta & 1) == 0) {
			/* Forwards was done first, now both d are the same. */
			int forwards_d = d;

			/* As soon as the lengths are not the same, the backwards traversal starts on a different diagonal, and
			 * c = k shifted by the difference in length.
			 */
			int k = c_to_k(c, delta);

			/* When the file sizes are very different, the traversal trees start on far distant diagonals.
			 * They don't necessarily meet straight on. See whether this backward value is also on a valid
			 * diagonal in kd_forward[], and match them if so. */
			if (k >= -forwards_d && k <= forwards_d) {
				/* Current c is on a diagonal that exists in kd_forward[]. If the two x positions have
				 * met or passed (backward walked onto or past forward), then we've found a midpoint / a
				 * mid-box. */
				int forward_x = kd_forward[k];
				int forward_y = xk_to_y(forward_x, k);
				debug("Compare %d to %d  k=%d  (x=%d,y=%d) to (x=%d,y=%d)\n",
				      forward_x, x, k,
				      forward_x, xk_to_y(forward_x, k), x, xc_to_y(x, c, delta));
				if (forward_x <= prev_x && forward_y <= prev_y
				    && forward_x >= x) {
					*meeting_snake = (struct diff_box){
						.left_start = x,
						.left_end = forward_x,
						.right_start = xc_to_y(x, c, delta),
						.right_end = xk_to_y(forward_x, k),
					};
					debug("HIT x=%u,%u - y=%u,%u\n",
					      meeting_snake->left_start,
					      meeting_snake->right_start,
					      meeting_snake->left_end,
					      meeting_snake->right_end);
					return;
				}
			}
		}
	}
}

/* Myers "Divide et Impera": tracing forwards from the start and backwards from the end to find a midpoint that divides
 * the problem into smaller chunks. Requires only linear amounts of memory. */
enum diff_rc diff_algo_myers_divide(const struct diff_algo_config *algo_config, struct diff_state *state)
{
	enum diff_rc rc = DIFF_RC_ENOMEM;
	struct diff_data *left = &state->left;
	struct diff_data *right = &state->right;

	debug("\n** %s\n", __func__);
	debug("left:\n");
	debug_dump(left);
	debug("right:\n");
	debug_dump(right);
	debug_dump_myers_graph(left, right, NULL);

	/* Allocate two columns of a Myers graph, one for the forward and one for the backward traversal. */
	unsigned int max = left->atoms.len + right->atoms.len;
	size_t kd_len = max + 1;
	size_t kd_buf_size = kd_len << 1;
	int *kd_buf = reallocarray(NULL, kd_buf_size, sizeof(int));
	if (!kd_buf)
		return DIFF_RC_ENOMEM;
	int i;
	for (i = 0; i < kd_buf_size; i++)
		kd_buf[i] = -1;
	int *kd_forward = kd_buf;
	int *kd_backward = kd_buf + kd_len;

	/* The 'k' axis in Myers spans positive and negative indexes, so point the kd to the middle.
	 * It is then possible to index from -max/2 .. max/2. */
	kd_forward += max/2;
	kd_backward += max/2;

	int d;
	struct diff_box mid_snake = {};
	for (d = 0; d <= (max/2); d++) {
		debug("-- d=%d\n", d);
		diff_divide_myers_forward(left, right, kd_forward, kd_backward, d, &mid_snake);
		if (!diff_box_empty(&mid_snake))
			break;
		diff_divide_myers_backward(left, right, kd_forward, kd_backward, d, &mid_snake);
		if (!diff_box_empty(&mid_snake))
			break;
	}

	if (diff_box_empty(&mid_snake)) {
		/* Divide and conquer failed to find a meeting point. Use the fallback_algo defined in the algo_config
		 * (leave this to the caller). This is just paranoia/sanity, we normally should always find a midpoint.
		 */
		debug(" no midpoint \n");
		rc = DIFF_RC_USE_DIFF_ALGO_FALLBACK;
		goto return_rc;
	} else {
		debug(" mid snake L: %u to %u of %u   R: %u to %u of %u\n",
		      mid_snake.left_start, mid_snake.left_end, left->atoms.len,
		      mid_snake.right_start, mid_snake.right_end, right->atoms.len);

		/* Section before the mid-snake.  */
		debug("Section before the mid-snake\n");

		struct diff_atom *left_atom = &left->atoms.head[0];
		unsigned int left_section_len = mid_snake.left_start;
		struct diff_atom *right_atom = &right->atoms.head[0];
		unsigned int right_section_len = mid_snake.right_start;

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
		/* else: left_section_len == 0 and right_section_len == 0, i.e. nothing before the mid-snake. */

		/* the mid-snake, identical data on both sides: */
		debug("the mid-snake\n");
		if (!diff_state_add_chunk(state, true,
					  &left->atoms.head[mid_snake.left_start],
					  mid_snake.left_end - mid_snake.left_start,
					  &right->atoms.head[mid_snake.right_start],
					  mid_snake.right_end - mid_snake.right_start))
			goto return_rc;

		/* Section after the mid-snake. */
		debug("Section after the mid-snake\n");
		debug("  left_end %u  right_end %u\n", mid_snake.left_end, mid_snake.right_end);
		debug("  left_count %u  right_count %u\n", left->atoms.len, right->atoms.len);
		left_atom = &left->atoms.head[mid_snake.left_end];
		left_section_len = left->atoms.len - mid_snake.left_end;
		right_atom = &right->atoms.head[mid_snake.right_end];
		right_section_len = right->atoms.len - mid_snake.right_end;

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
		/* else: left_section_len == 0 and right_section_len == 0, i.e. nothing after the mid-snake. */
	}

	rc = DIFF_RC_OK;

return_rc:
	free(kd_buf);
	debug("** END %s\n", __func__);
	return rc;
}

/* Myers Diff tracing from the start all the way through to the end, requiring quadratic amounts of memory. This can
 * fail if the required space surpasses algo_config->permitted_state_size. */
enum diff_rc diff_algo_myers(const struct diff_algo_config *algo_config, struct diff_state *state)
{
	/* do a diff_divide_myers_forward() without a _backward(), so that it walks forward across the entire
	 * files to reach the end. Keep each run's state, and do a final backtrace. */
	enum diff_rc rc = DIFF_RC_ENOMEM;
	struct diff_data *left = &state->left;
	struct diff_data *right = &state->right;

	debug("\n** %s\n", __func__);
	debug("left:\n");
	debug_dump(left);
	debug("right:\n");
	debug_dump(right);
	debug_dump_myers_graph(left, right, NULL);

	/* Allocate two columns of a Myers graph, one for the forward and one for the backward traversal. */
	unsigned int max = left->atoms.len + right->atoms.len;
	size_t kd_len = max + 1 + max;
	size_t kd_buf_size = kd_len * kd_len;
	debug("state size: %zu\n", kd_buf_size);
	if (kd_buf_size < kd_len /* overflow? */
	    || kd_buf_size * sizeof(int) > algo_config->permitted_state_size) {
		debug("state size %zu > permitted_state_size %zu, use fallback_algo\n",
		      kd_buf_size, algo_config->permitted_state_size);
		return DIFF_RC_USE_DIFF_ALGO_FALLBACK;
	}

	int *kd_buf = reallocarray(NULL, kd_buf_size, sizeof(int));
	if (!kd_buf)
		return DIFF_RC_ENOMEM;
	int i;
	for (i = 0; i < kd_buf_size; i++)
		kd_buf[i] = -1;

	/* The 'k' axis in Myers spans positive and negative indexes, so point the kd to the middle.
	 * It is then possible to index from -max .. max. */
	int *kd_origin = kd_buf + max;
	int *kd_column = kd_origin;

	int d;
	int backtrack_d = -1;
	int backtrack_k = 0;
	int k;
	int x, y;
	for (d = 0; d <= max; d++, kd_column += kd_len) {
		debug("-- d=%d\n", d);

		debug("-- %s d=%d\n", __func__, d);

		for (k = d; k >= -d; k -= 2) {
			if (k < -(int)right->atoms.len || k > (int)left->atoms.len) {
				/* This diagonal is completely outside of the Myers graph, don't calculate it. */
				if (k < -(int)right->atoms.len)
					debug(" %d k < -(int)right->atoms.len %d\n", k, -(int)right->atoms.len);
				else
					debug(" %d k > left->atoms.len %d\n", k, left->atoms.len);
				if (k < 0) {
					/* We are traversing negatively, and already below the entire graph, nothing will come
					 * of this. */
					debug(" break");
					break;
				}
				debug(" continue");
				continue;
			}

			debug("- k = %d\n", k);
			if (d == 0) {
				/* This is the initializing step. There is no prev_k yet, get the initial x from the top left of
				 * the Myers graph. */
				x = 0;
			} else {
				int *kd_prev_column = kd_column - kd_len;

				/* Favoring "-" lines first means favoring moving rightwards in the Myers graph.
				 * For this, all k should derive from k - 1, only the bottom most k derive from k + 1:
				 *
				 *      | d=   0   1   2
				 *  ----+----------------
				 *  k=  |
				 *   2  |             2,0 <-- from prev_k = 2 - 1 = 1
				 *      |            /
				 *   1  |         1,0
				 *      |        /
				 *   0  |  -->0,0     3,3
				 *      |       \\   /
				 *  -1  |         0,1 <-- bottom most for d=1 from prev_k = -1 + 1 = 0
				 *      |           \\
				 *  -2  |             0,2 <-- bottom most for d=2 from prev_k = -2 + 1 = -1
				 *
				 * Except when a k + 1 from a previous run already means a further advancement in the graph.
				 * If k == d, there is no k + 1 and k - 1 is the only option.
				 * If k < d, use k + 1 in case that yields a larger x. Also use k + 1 if k - 1 is outside the graph.
				 */
				if (k > -d && (k == d
					       || (k - 1 >= -(int)right->atoms.len
						   && kd_prev_column[k - 1] >= kd_prev_column[k + 1]))) {
					/* Advance from k - 1.
					 * From position prev_k, step to the right in the Myers graph: x += 1.
					 */
					int prev_k = k - 1;
					int prev_x = kd_prev_column[prev_k];
					x = prev_x + 1;
				} else {
					/* The bottom most one.
					 * From position prev_k, step to the bottom in the Myers graph: y += 1.
					 * Incrementing y is achieved by decrementing k while keeping the same x.
					 * (since we're deriving y from y = x - k).
					 */
					int prev_k = k + 1;
					int prev_x = kd_prev_column[prev_k];
					x = prev_x;
				}
			}

			/* Slide down any snake that we might find here. */
			while (x < left->atoms.len && xk_to_y(x, k) < right->atoms.len
			       && diff_atom_same(&left->atoms.head[x], &right->atoms.head[xk_to_y(x, k)]))
			       x++;
			kd_column[k] = x;

			if (DEBUG) {
				int fi;
				for (fi = d; fi >= k; fi-=2) {
					debug("kd_column[%d] = (%d, %d)\n", fi, kd_column[fi], kd_column[fi] - fi);
#if 0
					if (kd_column[fi] >= 0 && kd_column[fi] < left->atoms.len)
						debug_dump_atom(left, right, &left->atoms.head[kd_column[fi]]);
					else
						debug("\n");
					if (kd_column[fi]-fi >= 0 && kd_column[fi]-fi < right->atoms.len)
						debug_dump_atom(right, left, &right->atoms.head[kd_column[fi]-fi]);
					else
						debug("\n");
#endif
				}
			}

			if (x == left->atoms.len && xk_to_y(x, k) == right->atoms.len) {
				/* Found a path */
				backtrack_d = d;
				backtrack_k = k;
				debug("Reached the end at d = %d, k = %d\n",
				      backtrack_d, backtrack_k);
				break;
			}
		}

		if (backtrack_d >= 0)
			break;
	}

	debug_dump_myers_graph(left, right, kd_origin);

	/* backtrack. A matrix spanning from start to end of the file is ready:
	 *
	 *      | d=   0   1   2   3   4
	 *  ----+---------------------------------
	 *  k=  |
	 *   3  |
	 *      |
	 *   2  |             2,0
	 *      |            /
	 *   1  |         1,0     4,3
	 *      |        /       /   \
	 *   0  |  -->0,0     3,3     4,4 --> backtrack_d = 4, backtrack_k = 0
	 *      |        \   /   \
	 *  -1  |         0,1     3,4
	 *      |            \
	 *  -2  |             0,2
	 *      |
	 *
	 * From (4,4) backwards, find the previous position that is the largest, and remember it.
	 *
	 */
	for (d = backtrack_d, k = backtrack_k; d >= 0; d--) {
		x = kd_column[k];
		y = xk_to_y(x, k);

		/* When the best position is identified, remember it for that kd_column.
		 * That kd_column is no longer needed otherwise, so just re-purpose kd_column[0] = x and kd_column[1] = y,
		 * so that there is no need to allocate more memory.
		 */
		kd_column[0] = x;
		kd_column[1] = y;
		debug("Backtrack d=%d: xy=(%d, %d)\n",
		      d, kd_column[0], kd_column[1]);

		/* Don't access memory before kd_buf */
		if (d == 0)
			break;
		int *kd_prev_column = kd_column - kd_len;

		/* When y == 0, backtracking downwards (k-1) is the only way.
		 * When x == 0, backtracking upwards (k+1) is the only way.
		 *
		 *      | d=   0   1   2   3   4
		 *  ----+---------------------------------
		 *  k=  |
		 *   3  |
		 *      |                ..y == 0
		 *   2  |             2,0
		 *      |            /
		 *   1  |         1,0     4,3
		 *      |        /       /   \
		 *   0  |  -->0,0     3,3     4,4 --> backtrack_d = 4, backtrack_k = 0
		 *      |        \   /   \
		 *  -1  |         0,1     3,4
		 *      |            \
		 *  -2  |             0,2__
		 *      |                  x == 0
		 */
		debug("prev[k-1] = %d,%d  prev[k+1] = %d,%d\n",
		      kd_prev_column[k-1], xk_to_y(kd_prev_column[k-1],k-1),
		      kd_prev_column[k+1], xk_to_y(kd_prev_column[k+1],k+1));
		if (y == 0
		    || (x > 0 && kd_prev_column[k - 1] >= kd_prev_column[k + 1])) {
			k = k - 1;
			debug("prev k=k-1=%d x=%d y=%d\n",
			      k, kd_prev_column[k], xk_to_y(kd_prev_column[k], k));
		} else {
			k = k + 1;
			debug("prev k=k+1=%d x=%d y=%d\n",
			      k, kd_prev_column[k], xk_to_y(kd_prev_column[k], k));
		}
		kd_column = kd_prev_column;
	}

	/* Forwards again, this time recording the diff chunks.
	 * Definitely start from 0,0. kd_column[0] may actually point to the bottom of a snake starting at 0,0 */
	x = 0;
	y = 0;

	kd_column = kd_origin;
	for (d = 0; d <= backtrack_d; d++, kd_column += kd_len) {
		int next_x = kd_column[0];
		int next_y = kd_column[1];
		debug("Forward track from xy(%d,%d) to xy(%d,%d)\n",
		      x, y, next_x, next_y);

		struct diff_atom *left_atom = &left->atoms.head[x];
		int left_section_len = next_x - x;
		struct diff_atom *right_atom = &right->atoms.head[y];
		int right_section_len = next_y - y;

		rc = DIFF_RC_ENOMEM;
		if (left_section_len && right_section_len) {
			/* This must be a snake slide.
			 * Snake slides have a straight line leading into them (except when starting at (0,0)). Find
			 * out whether the lead-in is horizontal or vertical:
			 *
			 *     left
			 *  ---------->
			 *  |
			 * r|   o-o        o
			 * i|      \       |
			 * g|       o      o
			 * h|        \      \
			 * t|         o      o
			 *  v
			 *
			 * If left_section_len > right_section_len, the lead-in is horizontal, meaning first
			 * remove one atom from the left before sliding down the snake.
			 * If right_section_len > left_section_len, the lead-in is vetical, so add one atom from
			 * the right before sliding down the snake. */
			if (left_section_len == right_section_len + 1) {
				if (!diff_state_add_chunk(state, true,
							  left_atom, 1,
							  right_atom, 0))
					goto return_rc;
				left_atom++;
				left_section_len--;
			} else if (right_section_len == left_section_len + 1) {
				if (!diff_state_add_chunk(state, true,
							  left_atom, 0,
							  right_atom, 1))
					goto return_rc;
				right_atom++;
				right_section_len--;
			} else if (left_section_len != right_section_len) {
				/* The numbers are making no sense. Should never happen. */
				rc = DIFF_RC_USE_DIFF_ALGO_FALLBACK;
				goto return_rc;
			}

			if (!diff_state_add_chunk(state, true,
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

		x = next_x;
		y = next_y;
	}

	rc = DIFF_RC_OK;

return_rc:
	free(kd_buf);
	debug("** END %s rc=%d\n", __func__, rc);
	return rc;
}
