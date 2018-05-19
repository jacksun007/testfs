SRCDIR := ..
vpath %.c $(SRCDIR)
vpath %.h $(SRCDIR)
DEFINES := -DCACHE_INVALIDATE
INCLUDES := -I/usr/lib/swi-prolog/include/
CFLAGS := -g -Wall $(DEFINES) $(INCLUDES)
OBJECTS := $(COMMON_OBJECTS) rv/rv.o rv/block_diff.o rv/rv_interval.o \
	rv/rv_testfs.o rv/rbtree.o rv/prolog.o
DEP_SOURCES := $(OBJECTS:.o=.c) $(addsuffix .c, $(PROGS))
SOURCES := $(addprefix ../, $(DEP_SOURCES))
RVDIR := $(SRCDIR)/rv

all: setup $(PROGS)

mktestfs: mktestfs.o $(OBJECTS)
	swipl-ld -o $@ $(LOADLIBES) $(CFLAGS) -ld g++ -goal true $^ $(RVDIR)/testfs.pl

testfs: testfs.o $(OBJECTS)
	swipl-ld -o $@ $(LOADLIBES) $(CFLAGS) -ld g++ -goal true $^ $(RVDIR)/testfs.pl

clean:
	rm -f *.o $(PROGS) *.exe *~ reference-disk depend.mk
	rm -rf rv

.PHONY: setup
setup:
	-mkdir rv 2> /dev/null

depend:
	$(CC) -MM $(INCLUDES) $(SOURCES) > depend.mk

ifeq (depend.mk,$(wildcard depend.mk))
include depend.mk
endif
