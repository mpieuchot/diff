.PATH:${.CURDIR}/../lib

.include "diff-version.mk"

PROG=	diff
SRCS= \
	diff.c \
	diff_atomize_text.c \
	diff_main.c \
	diff_myers.c \
	diff_patience.c \
	diff_output.c \
	diff_output_plain.c \
	diff_output_unidiff.c \
	${END}
MAN =	${PROG}.1

CPPFLAGS = -I${.CURDIR}/../include -I${.CURDIR}/../lib

.if defined(PROFILE)
LDADD = -lutil_p -lz_p -lc_p
.else
LDADD = -lutil -lz
.endif

.if ${DIFF_RELEASE} != "Yes"
NOMAN = Yes
.endif

realinstall:
	${INSTALL} ${INSTALL_COPY} -o ${BINOWN} -g ${BINGRP} \
	-m ${BINMODE} ${PROG} ${BINDIR}/${PROG}

dist:
	mkdir ../diff-${DIFF_VERSION}/diff
	cp ${SRCS} ${MAN} ../diff-${DIFF_VERSION}/diff

.include <bsd.prog.mk>
