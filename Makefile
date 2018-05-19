# 
# Master Makefile for testfs
#
# Author: Kuei Sun
# E-mail: <kuei.sun@utoronto.ca>
#
# University of Toronto

VERSION := 0.1
ZIPFILE := testfs_v$(VERSION).tar.gz
BUILDS := full no-recon no-checking manual vanilla

export PROGS:=mktestfs testfs
export COMMON_OBJECTS:=bitmap.o block.o super.o inode.o dir.o file.o tx.o common.o
export ALL_OBJECTS:=$(addsuffix .o, $(PROGS)) $(COMMON_OBJECTS)
export COMMON_SOURCES:=$(COMMON_OBJECTS:.o=.c)
export ALL_SOURCES:=$(ALL_OBJECTS:.o=.c)
export LOADLIBES:=-lpopt
export CC:=gcc
export COMMON_FLAGS := -g -Wall -Wextra -Werror -Wno-unused-parameter
CLEANERS := $(addsuffix .clean, $(BUILDS))

all: $(BUILDS)

clean: $(CLEANERS)
	rm -f testfs*.tar.gz TAGS 
	rm -rf *~

$(BUILDS):
	cd $@ && $(MAKE) depend && $(MAKE) all

$(CLEANERS):
	cd $(basename $@) && $(MAKE) clean

zip:
	tar cvzf $(ZIPFILE) *.c *.h rv/*.c rv/*.h rv/testfs.pl \
	corruption/benchmark.py Makefile $(addsuffix /Makefile, $(BUILDS))

.PHONY: zip clean $(BUILDS) $(CLEANERS)

