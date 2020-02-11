/* Auto-reallocating array for arbitrary member types. */
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

#ifdef __linux__
/* stupid shims to compile and test on linux */
#include <strings.h>
static inline void *reallocarray(void *buf, size_t n, size_t member_size)
{
	return realloc(buf, n * member_size);
}
static inline void *recallocarray(void *buf, size_t oldn, size_t n, size_t member_size)
{
	buf = reallocarray(buf, n, member_size);
	bzero(((char*)buf) + (oldn * member_size), (n - oldn) * member_size);
	return buf;
}
#endif


/* Usage:
 *
 * ARRAYLIST(any_type_t) list;
 * // OR
 * typedef ARRAYLIST(any_type_t) any_type_list_t;
 * any_type_list_t list;
 *
 * ARRAYLIST_INIT(list, 128); // < number of (at first unused) members to add on each realloc
 * any_type_t *x;
 * while (bar) {
 *         ARRAYLIST_ADD(x, list); // < enlarges the allocated array as needed; list.head may change due to realloc
 *         if (!x)
 *                 abort();
 *         *x = random_foo_value;
 * }
 * for (i = 0; i < list.len; i++)
 *         printf("%s", foo_to_str(list.head[i]));
 * ARRAYLIST_FREE(list);
 */
#define ARRAYLIST(MEMBER_TYPE) \
	struct { \
		MEMBER_TYPE *head; \
		unsigned int len; \
		unsigned int allocated; \
		unsigned int alloc_blocksize; \
	}

#define ARRAYLIST_INIT(ARRAY_LIST, ALLOC_BLOCKSIZE) do { \
		(ARRAY_LIST).head = NULL; \
		(ARRAY_LIST).len = 0; \
		(ARRAY_LIST).allocated = 0; \
		(ARRAY_LIST).alloc_blocksize = ALLOC_BLOCKSIZE; \
	} while(0)

#define ARRAYLIST_ADD(NEW_ITEM_P, ARRAY_LIST) do { \
		if ((ARRAY_LIST).len && !(ARRAY_LIST).allocated) { \
			NEW_ITEM_P = NULL; \
			break; \
		} \
		if ((ARRAY_LIST).head == NULL || (ARRAY_LIST).allocated < (ARRAY_LIST).len + 1) { \
			(ARRAY_LIST).allocated += (ARRAY_LIST).alloc_blocksize ? : 8; \
			(ARRAY_LIST).head = recallocarray((ARRAY_LIST).head, (ARRAY_LIST).len, \
							  (ARRAY_LIST).allocated, sizeof(*(ARRAY_LIST).head)); \
		}; \
		if (!(ARRAY_LIST).head || (ARRAY_LIST).allocated < (ARRAY_LIST).len + 1) { \
			NEW_ITEM_P = NULL; \
			break; \
		} \
		(NEW_ITEM_P) = &(ARRAY_LIST).head[(ARRAY_LIST).len]; \
		(ARRAY_LIST).len++; \
	} while (0)

#define ARRAYLIST_CLEAR(ARRAY_LIST) \
	(ARRAY_LIST).len = 0

#define ARRAYLIST_FREE(ARRAY_LIST) \
	do { \
		if ((ARRAY_LIST).head && (ARRAY_LIST).allocated) \
			free((ARRAY_LIST).head); \
		ARRAYLIST_INIT(ARRAY_LIST, (ARRAY_LIST).alloc_blocksize); \
	} while(0)
