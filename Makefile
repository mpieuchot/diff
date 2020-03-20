# $OpenBSD$

PROG=		diff
SRCS=		diff.c diff_atomize_text.c diff_main.c diff_myers.c \
		diff_patience.c diff_output.c

CFLAGS+=	-Wstrict-prototypes -Wunused-variable

.include <bsd.prog.mk>
