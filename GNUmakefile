# diff-portable

PROG=		diff
SRCS=		diff.c diff_atomize_text.c diff_main.c diff_myers.c \
		diff_patience.c

CFLAGS+=	-Wstrict-prototypes -Wunused-variable


# Compat sources
VPATH=		$(CURDIR)/compat
SRCS+=		getprogname_linux.c recallocarray.c
CFLAGS+=	-I$(CURDIR)/compat/include


# Shouldn't need to change anything below
all: $(PROG)

$(PROG): $(SRCS:.c=.o)

.c.o:
	$(CC) -c $(CFLAGS) $<

.PHONY: clean
clean:
	rm -f $(PROG) *.o
