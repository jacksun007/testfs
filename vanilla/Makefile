vpath %.c ..
vpath %.h ..
DEFINES := -DDISABLE_PROLOG -DCACHE_INVALIDATE
INCLUDES := 
CFLAGS := $(COMMON_FLAGS) $(DEFINES) $(INCLUDES)
OBJECTS := $(COMMON_OBJECTS) rv/rv.o rv/block_diff.o rv/rv_interval.o \
	rv/rv_testfs.o rv/rbtree.o
DEP_SOURCES := $(OBJECTS:.o=.c) $(addsuffix .c, $(PROGS))
SOURCES := $(addprefix ../, $(DEP_SOURCES))

all: setup $(PROGS)

mktestfs: mktestfs.o $(OBJECTS)
	$(CC) -o $@ $(CFLAGS) $^ $(LOADLIBES) 

testfs: testfs.o $(OBJECTS)
	$(CC) -o $@ $(CFLAGS) $^ $(LOADLIBES) 

clean:
	rm -f depend.mk *.o $(PROGS) *.exe *~ reference-disk
	rm -rf rv

.PHONY: setup
setup:
	-mkdir rv 2> /dev/null	   

depend:
	$(CC) -MM $(INCLUDES) $(SOURCES) > depend.mk

ifeq (depend.mk,$(wildcard depend.mk))
include depend.mk
endif

