# SPDX-License-Identifier: GPL-2.0

.PHONY: clean

TARGETS = main idr-test multiorder xarray maple cc-xarray
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

# Use SHIFT=6 for production-sized 6->0 and 7->1 split boundaries.
# Run make clean before changing SHIFT; targets share node-cache objects.
cc-xarray: cc-xarray.o cc-xarray-shared.o $(SHARED_OFILES)
cc-xarray.o cc-xarray-shared.o: $(SHARED_DEPS) \
	../../../include/linux/cc_xarray.h ../shared/linux/cc_xarray.h \
	../shared/linux/irqflags.h
cc-xarray-shared.o: ../../../lib/cc_xarray.c

maple: $(CORE_OFILES) maple.o

multiorder: multiorder.o $(CORE_OFILES)

clean:
	$(RM) $(TARGETS) *.o radix-tree.c idr.c generated/*

$(OFILES): $(SHARED_DEPS) *.h
