# pool: a C parallel-for, measured on BLAKE3 pieces (b3tree, asm kernels).
# parhash-tsan is the same program under ThreadSanitizer, for the atomics.

CC     = gcc
CFLAGS = -O3 -march=native -std=gnu11 -Wall -Wextra -Werror -pthread
B3     = ../b3tree/build/b3tree-asm.o ../b3tree/build/kern-asm.o

all: build/parhash build/parhash-tsan rust/target/release/parhash

$(B3):
	$(MAKE) -C ../b3tree $(B3:../b3tree/%=%)

build/parhash: parhash.c pool.c pool.h $(B3)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ parhash.c pool.c $(B3)

build/parhash-tsan: parhash.c pool.c pool.h $(B3)
	@mkdir -p build
	$(CC) $(CFLAGS:-O3=-O1) -g -fsanitize=thread -o $@ parhash.c pool.c $(B3)

rust/target/release/parhash: rust/Cargo.toml rust/src/main.rs
	cd rust && cargo build --release --offline

clean:
	rm -rf build rust/target

.PHONY: all clean
