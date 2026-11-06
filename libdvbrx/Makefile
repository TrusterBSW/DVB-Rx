CC ?= gcc
MARCH ?= native
CFLAGS ?= -O3 -march=$(MARCH) -fno-math-errno -Wall -Wextra -std=gnu11 -ffp-contract=fast -fcx-limited-range -D_GNU_SOURCE
CPPFLAGS += -Iinclude
DVBRX_VERSION := $(shell cat ../VERSION 2>/dev/null || echo unknown)
LDLIBS += -lfftw3f -lm -lpthread

SRC := src/frontend.c src/bb.c src/bch.c src/ldpc.c src/demap.c src/pl.c src/rx.c src/rxmt.c src/dvbs.c src/conv.c src/outer.c src/dvbc.c src/fft.c src/resamp.c src/dvbt.c src/dvbt2.c src/chan.c
OBJ := $(SRC:src/%.c=build/%.o)

TESTS := $(patsubst ../tests/%.c,build/%,$(wildcard ../tests/test_*.c))

# dvbrx's SDR drivers, each built when its library is installed: UHD
# (USRP B2xx, through the UHD C API), libbladeRF (bladeRF), libhackrf (HackRF).
HACKRF_H := $(wildcard /usr/include/libhackrf/hackrf.h)
SDR_FLAGS := $(if $(wildcard /usr/include/uhd.h),-DHAVE_UHD,) $(if $(wildcard /usr/include/libbladeRF.h),-DHAVE_BLADERF,) $(if $(HACKRF_H),-DHAVE_HACKRF,)
SDR_FLAGS += $(if $(HACKRF_H),$(if $(shell grep -l hackrf_get_m0_state $(HACKRF_H)),-DHACKRF_HAVE_M0_STATE,),)
SDR_LIBS := -ldl # the SDR libraries are loaded at run time (tools/sdr_dl.h)
SDR_SRC := $(if $(wildcard /usr/include/uhd.h),tools/sdr_uhd.c,) $(if $(wildcard /usr/include/libbladeRF.h),tools/sdr_bladerf.c,) $(if $(HACKRF_H),tools/sdr_hackrf.c,)

all: build/libdvbrx.so build/libdvbrx.a build/fe_bench build/dvbrx_file build/dvbrx $(TESTS)

build/%.o: src/%.c include/dvbrx/*.h src/*.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

build/libdvbrx.so: $(OBJ)
	$(CC) -shared -o $@ $^ $(LDLIBS)

build/libdvbrx.a: $(OBJ)
	ar rcs $@ $^

build/fe_bench: tools/fe_bench.c build/libdvbrx.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libdvbrx.a -o $@ $(LDLIBS)

build/dvbrx_file: tools/dvbrx_file.c build/libdvbrx.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libdvbrx.a -o $@ $(LDLIBS)

build/dvbrx: ../VERSION tools/dvbrx.c tools/psi.c tools/psi.h tools/sdr.h tools/sdr_dl.h $(SDR_SRC) build/libdvbrx.a ../kernel/dvbrx_ioctl.h
	$(CC) $(CPPFLAGS) -I../kernel -DDVBRX_VERSION='"$(DVBRX_VERSION)"' $(CFLAGS) $(SDR_FLAGS) tools/dvbrx.c tools/psi.c $(SDR_SRC) build/libdvbrx.a -o $@ $(LDLIBS) $(SDR_LIBS)

build/test_%: ../tests/test_%.c build/libdvbrx.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libdvbrx.a -o $@ $(LDLIBS)

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
