CC ?= gcc
CFLAGS ?= -O3 -march=native -fno-math-errno -Wall -Wextra -std=gnu11 -ffp-contract=fast -fcx-limited-range -D_GNU_SOURCE
CPPFLAGS += -Iinclude
LDLIBS += -lfftw3f -lm -lpthread

SRC := src/frontend.c src/bb.c src/bch.c src/ldpc.c src/demap.c src/pl.c src/rx.c src/rxmt.c src/dvbs.c src/conv.c src/outer.c src/dvbc.c src/fft.c src/resamp.c src/dvbt.c src/dvbt2.c src/chan.c
OBJ := $(SRC:src/%.c=build/%.o)

TESTS := $(patsubst ../tests/%.c,build/%,$(wildcard ../tests/test_*.c))

# dvbrx talks to USRPs through the UHD C API when it is installed.
UHD_FLAGS := $(if $(wildcard /usr/include/uhd.h),-DHAVE_UHD,)
UHD_LIBS := $(if $(wildcard /usr/include/uhd.h),-luhd,)

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

build/dvbrx: tools/dvbrx.c tools/psi.c tools/psi.h build/libdvbrx.a ../kernel/dvbrx_ioctl.h
	$(CC) $(CPPFLAGS) -I../kernel $(CFLAGS) $(UHD_FLAGS) tools/dvbrx.c tools/psi.c build/libdvbrx.a -o $@ $(LDLIBS) $(UHD_LIBS)

build/test_%: ../tests/test_%.c build/libdvbrx.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libdvbrx.a -o $@ $(LDLIBS)

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
