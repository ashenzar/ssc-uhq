# Native SSC encoder (ssc-uhq O5): libopus v1.1 + patches/ + src/ssc_encoder.c,
# built for the host. Samsung's tables come from the user's blob at build time
# (tools/ssc-extract-tables.py -> build/ssc_tables.h, never committed: D28).
CC      ?= gcc
OPUS    := ../third_party/opus
H       := ../src
CFLAGS  := -O2 -g -std=gnu99 -Wall -Wno-unused-function -Wno-maybe-uninitialized
REFDEF  := -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DCUSTOM_MODES -DVAR_ARRAYS \
           -DHAVE_LRINT -DHAVE_LRINTF -DSSC_NO_TRACE
BLOB    ?= $(HOME)/src/ssc-blob/s24u/libScalable_Encoder.so
B       := build
SRC     := $(B)/src
PATCHES := $(sort $(wildcard ../patches/*.patch))
INC     := -I$(SRC)/celt -I$(OPUS)/include -I$(OPUS) -I$(H) -I$(B) -I.

# ssc_encoder.c includes celt_encoder.c; files a patch creates are added.
CELT_C  := $(notdir $(filter-out %/opus_custom_demo.c %/celt_encoder.c,$(wildcard $(OPUS)/celt/*.c)))
CELT_C  += $(notdir $(shell awk '/^\+\+\+ b\/celt\/.*\.c$$/{f=$$2} /^@@ -0,0 /{if(f)print f; f=""}' $(PATCHES) /dev/null))
OBJ     := $(addprefix $(B)/obj/,$(CELT_C:.c=.o)) $(B)/obj/ssc_encoder.o $(B)/obj/ssc_native.o

all: $(B)/libsscenc.a $(B)/ssc_native_helper $(B)/ssc_native_bench $(B)/ssc_native_roundtrip $(B)/ssc_native_decode

$(SRC)/.stamp: $(PATCHES) $(wildcard $(OPUS)/celt/*.[ch])
	rm -rf $(SRC) && mkdir -p $(SRC)
	cp -r $(OPUS)/celt $(SRC)/celt
	for p in $(PATCHES); do patch -s -p1 -d $(SRC) < $$p || exit 1; done
	touch $@

$(B)/ssc_tables.h: $(BLOB) ../tools/ssc-extract-tables.py | $(B)
	../tools/ssc-extract-tables.py $(BLOB) $@

$(B)/obj/%.o: $(SRC)/.stamp | $(B)/obj
	$(CC) $(CFLAGS) $(REFDEF) $(INC) -c $(SRC)/celt/$*.c -o $@
$(B)/obj/ssc_encoder.o: $(H)/ssc_encoder.c $(wildcard $(H)/*.h) $(SRC)/.stamp | $(B)/obj
	$(CC) $(CFLAGS) $(REFDEF) $(INC) -c $< -o $@
$(B)/obj/ssc_native.o: ssc_native.c ssc_native.h $(B)/ssc_tables.h $(SRC)/.stamp | $(B)/obj
	$(CC) $(CFLAGS) $(REFDEF) $(INC) -c $< -o $@
$(B)/libsscenc.a: $(OBJ)
	rm -f $@ && ar rcs $@ $^
$(B)/ssc_native_helper: ssc_native_helper.c ssc_native.h $(B)/libsscenc.a
	$(CC) $(CFLAGS) -I. -o $@ $< $(B)/libsscenc.a -lm
$(B)/ssc_native_bench: ssc_native_bench.c ssc_native.h $(B)/libsscenc.a
	$(CC) $(CFLAGS) -I. -o $@ $< $(B)/libsscenc.a -lm
$(B)/ssc_native_roundtrip $(B)/ssc_native_decode: $(B)/%: %.c ssc_native_dec.h ssc_native.h $(H)/ssc_header.h $(B)/libsscenc.a
	$(CC) $(CFLAGS) $(REFDEF) $(INC) -o $@ $< $(B)/libsscenc.a -lm
$(B) $(B)/obj:
	mkdir -p $@

clean:
	rm -rf $(B)
.PHONY: all clean
