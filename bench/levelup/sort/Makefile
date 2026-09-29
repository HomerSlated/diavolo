# The read plan's sort in isolation; plans come from
#   ACCBENCH_PLAN_DUMP=FILE ../../access/c/build/accbench-gcc rawsort IMAGE

CFLAGS = -O3 -march=native -std=gnu11 -Wall -Wextra -Werror

all: build/sortbench-gcc build/sortbench-clang rust/target/release/sortbench

build/sortbench-gcc: sortbench.c radix.h
	@mkdir -p build
	gcc $(CFLAGS) -o $@ $<

build/sortbench-clang: sortbench.c radix.h
	@mkdir -p build
	clang $(CFLAGS) -o $@ $<

rust/target/release/sortbench: rust/Cargo.toml rust/src/main.rs
	cd rust && cargo build --release --offline

clean:
	rm -rf build rust/target

.PHONY: all clean
