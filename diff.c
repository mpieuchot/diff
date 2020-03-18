/*	$OpenBSD$	*/

/*
 * Copyright (c) 2018 Martin Pieuchot
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
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "diff_main.h"

#define DEFAULT_CONTEXT	3

#define F_UNIFIED	(1 << 0)

__dead void	 usage(void);
int		 diffreg(char *, char *, int, int);
char		*mmapfile(const char *, struct stat *);

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
	struct diff_input_info info = {
		.left_path = file1,
		.right_path = file2,
	};

	str1 = mmapfile(file1, &st1);
	str2 = mmapfile(file2, &st2);

	if (flags & F_UNIFIED)
		diff_unidiff(stdout, &diff_config, &info, str1, st1.st_size,
		    str2, st2.st_size, context);
	else
		diff_plain(stdout, &diff_config, &info, str1, st1.st_size,
		    str2, st2.st_size);

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
