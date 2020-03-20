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

/* Auto-reallocating array for arbitrary member types. */

#pragma once

/* Usage:
 *
 * ARRAYLIST_HEAD(head_type, any_type) list;
 *
 * // number of (at first unused) members to add on each realloc
 * ARRAYLIST_INIT(list, 128);
 *
 * 	struct any_type *x;
 * 	while (bar) {
 * 	        // enlarges the allocated array as needed;
 * 	        // list.head may change due to realloc
 * 	        ARRAYLIST_ADD(x, list);
 * 	        if (x == NULL)
 * 	                abort();
 * 	        *x = random_foo_value;
 * 	}
 * for (i = 0; i < list.len; i++)
 *         printf("%s", foo_to_str(list.head[i]));
 * ARRAYLIST_FREE(list);
 */
#define ARRAYLIST_HEAD(name, type)					\
	struct name { 							\
		struct type *head;					\
		unsigned int len;					\
		unsigned int allocated;					\
		unsigned int blocksize;					\
	}

#define ARRAYLIST_INIT(alh, nelems) do { 				\
		(alh).head = NULL; 					\
		(alh).len = 0;						\
		(alh).allocated = 0;					\
		(alh).blocksize = (nelems);				\
} while(0)

#define ARRAYLIST_ADD(elm, alh) do { 					\
		if ((alh).len && (alh).allocated == 0) {		\
			elm = NULL; 					\
			break; 						\
		} 							\
		if ((alh).head == NULL ||				\
		    (alh).allocated < (alh).len + 1) { 			\
			(alh).allocated +=				\
			    (alh).blocksize ? : 8; 			\
			(alh).head = recallocarray((alh).head, 		\
			    (alh).len, (alh).allocated,			\
			    sizeof(*(alh).head)); 			\
		} 							\
		if ((alh).head == NULL ||				\
		    (alh).allocated < (alh).len + 1) { 			\
			elm = NULL; 					\
			break; 						\
		} 							\
		(elm) = &(alh).head[(alh).len];				\
		(alh).len++; 						\
} while (0)

#define ARRAYLIST_CLEAR(alh)						\
	(alh).len = 0

#define ARRAYLIST_FREE(alh) do { 					\
		if ((alh).head != NULL && (alh).allocated > 0) 		\
			free((alh).head); 				\
		ARRAYLIST_INIT(alh, (alh).blocksize); 			\
} while(0)
