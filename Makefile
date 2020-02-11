PROG=	diff
SRCS= \
	diff.c \
	diff_atomize_text.c \
	diff_main.c \
	diff_myers.c \
	diff_patience.c \
	diff_output.c \
	${END}

.if defined(PROFILE)
LDADD = -lutil_p -lz_p -lc_p
.else
LDADD = -lutil -lz
.endif

.include <bsd.prog.mk>
