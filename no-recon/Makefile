vpath %.c ..
vpath %.h ..
DEFINES := -DDISABLE_RECON -DDISABLE_PROMPT -DDISABLE_OSYNC
INCLUDES := 
CFLAGS := $(COMMON_FLAGS) $(DEFINES) $(INCLUDES)
SOURCES := $(addprefix ../, $(ALL_SOURCES))

all: $(PROGS)

mktestfs: mktestfs.o $(COMMON_OBJECTS)
	$(CC) -o $@ $(CFLAGS) $^ $(LOADLIBES) 

testfs: testfs.o $(COMMON_OBJECTS)
	$(CC) -o $@ $(CFLAGS) $^ $(LOADLIBES) 

depend:
	$(CC) -MM $(INCLUDES) $(SOURCES) > depend.mk

clean:
	rm -f *.o $(PROGS) *.exe *~ reference-disk depend.mk

ifeq (depend.mk,$(wildcard depend.mk))
include depend.mk
endif
