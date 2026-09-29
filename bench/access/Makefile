# Builds the three implementations of accbench. Run as your own user, not
# under doas: cargo needs your ~/.cargo for its offline crate cache.

CFLAGS = -O3 -march=native -std=gnu11 -Wall -Wextra -Werror -pthread
LDLIBS = -lext2fs -lcom_err

# BLAKE3 tree hashing, radix sort and thread pool from ../levelup, compiled
# into each binary by that binary's compiler; the kernels are upstream's asm.
L      = ../levelup
LSRC   = $(L)/b3tree/b3tree.c $(L)/pool/pool.c \
         $(L)/b3tree/vendor/blake3_avx2_x86-64_unix.S $(L)/b3tree/vendor/blake3_sse41_x86-64_unix.S
LDEPS  = $(LSRC) $(L)/b3tree/b3tree.h $(L)/pool/pool.h $(L)/sort/radix.h

all: c/build/accbench-gcc c/build/accbench-clang rust/target/release/accbench

c/build/accbench-gcc: c/accbench.c $(LDEPS)
	@mkdir -p c/build
	gcc $(CFLAGS) -o $@ $< $(LSRC) $(LDLIBS)

c/build/accbench-clang: c/accbench.c $(LDEPS)
	@mkdir -p c/build
	clang $(CFLAGS) -o $@ $< $(LSRC) $(LDLIBS)

# ThreadSanitizer build, for --threads
c/build/accbench-tsan: c/accbench.c $(LDEPS)
	@mkdir -p c/build
	gcc $(CFLAGS:-O3=-O1) -g -fsanitize=thread -o $@ $< $(LSRC) $(LDLIBS)

rust/target/release/accbench: rust/Cargo.toml rust/src/*.rs rust/src/sys/*.rs
	cd rust && cargo build --release --offline

clean:
	rm -rf c/build rust/target

.PHONY: all clean
