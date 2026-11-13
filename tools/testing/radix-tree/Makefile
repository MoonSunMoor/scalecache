# SPDX-License-Identifier: GPL-2.0

.PHONY: clean

TARGETS = main idr-test multiorder xarray maple cc-shadow-reclaim
CORE_OFILES = $(SHARED_OFILES) xarray.o maple.o test.o
OFILES = main.o $(CORE_OFILES) regression1.o regression2.o \
	 regression3.o regression4.o tag_check.o multiorder.o idr-test.o \
	iteration_check.o iteration_check_2.o benchmark.o

targets: generated/map-shift.h generated/bit-length.h $(TARGETS)

include ../shared/shared.mk

main:	$(OFILES)

xarray.o: ../../../lib/test_xarray.c
idr-test.o: ../../../lib/test_ida.c
idr-test: idr-test.o $(CORE_OFILES)

xarray: $(CORE_OFILES) xarray.o

cc-shadow-reclaim: cc-shadow-reclaim.o $(SHARED_OFILES)
cc-shadow-reclaim.o: $(SHARED_DEPS) ../../../lib/cc_xarray.c \
	../../../include/linux/cc_xarray.h ../shared/linux/cc_xarray.h \
	../shared/linux/irqflags.h generated/cc-shadow-list-lru.h \
	generated/cc-shadow-workingset.h generated/cc-shadow-truncate.h \
	generated/cc-shadow-swap.h generated/cc-shadow-mapping.h \
	../../../include/linux/folio_batch.h

WORKINGSET_SOURCE ?= ../../../mm/workingset.c
TRUNCATE_SOURCE ?= ../../../mm/truncate.c
generated/cc-shadow-list-lru.h: cc-shadow-sources.py ../../../mm/list_lru.c
	@mkdir -p generated
	python3 $< lru ../../../mm/list_lru.c > $@
generated/cc-shadow-workingset.h: cc-shadow-sources.py $(WORKINGSET_SOURCE)
	@mkdir -p generated
	python3 $< workingset $(WORKINGSET_SOURCE) > $@
generated/cc-shadow-truncate.h: cc-shadow-sources.py $(TRUNCATE_SOURCE)
	@mkdir -p generated
	python3 $< truncate $(TRUNCATE_SOURCE) > $@
generated/cc-shadow-swap.h: cc-shadow-sources.py ../../../mm/swap.c
	@mkdir -p generated
	python3 $< swap ../../../mm/swap.c > $@
generated/cc-shadow-mapping.h: cc-shadow-sources.py ../../../include/linux/pagemap.h
	@mkdir -p generated
	python3 $< mapping ../../../include/linux/pagemap.h > $@

maple: $(CORE_OFILES) maple.o

multiorder: multiorder.o $(CORE_OFILES)

clean:
	$(RM) $(TARGETS) *.o radix-tree.c idr.c generated/*

$(OFILES): $(SHARED_DEPS) *.h
