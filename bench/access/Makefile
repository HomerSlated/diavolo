# Builds the three implementations of accbench. Run as your own user, not
# under doas: cargo needs your ~/.cargo for its offline crate cache.

CFLAGS = -O3 -march=native -std=gnu11 -Wall -Wextra -Werror
LDLIBS = -lext2fs -lcom_err

all: c/build/accbench-gcc c/build/accbench-clang rust/target/release/accbench

c/build/accbench-gcc: c/accbench.c
	@mkdir -p c/build
	gcc $(CFLAGS) -o $@ $< $(LDLIBS)

c/build/accbench-clang: c/accbench.c
	@mkdir -p c/build
	clang $(CFLAGS) -o $@ $< $(LDLIBS)

rust/target/release/accbench: rust/Cargo.toml rust/src/*.rs rust/src/sys/*.rs
	cd rust && cargo build --release --offline

clean:
	rm -rf c/build rust/target

.PHONY: all clean
