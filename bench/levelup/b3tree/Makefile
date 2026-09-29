# b3tree: our BLAKE3 tree layer over upstream kernels, in three kernel flavours:
#   asm   the x86-64 assembly the Rust blake3 crate links (AVX2 + SSE4.1)
#   intr  upstream's C intrinsics for the same (any C compiler with <immintrin.h>)
#   port  upstream's portable C only -- what an Amiga or any non-x86 would get
# Kernel symbols are renamed b3v_* so the system libblake3 (the proof's
# reference) cannot interpose on them, or they on it.

CC       = gcc
OPT      = -O3 -march=native
CFLAGS   = $(OPT) -std=gnu11 -Wall -Wextra -Werror
VFLAGS   = $(OPT) -std=gnu11 -w
K        = hash_many_avx512 compress_in_place_avx512 compress_xof_avx512 xof_many_avx512 \
           hash_many_avx2 hash_many_sse41 compress_in_place_sse41 compress_xof_sse41 \
           hash_many_sse2 compress_in_place_sse2 compress_xof_sse2 \
           hash_many_portable compress_in_place_portable compress_xof_portable
RENAME   = $(foreach k,$(K),-Dblake3_$(k)=b3v_$(k) -D_blake3_$(k)=_b3v_$(k))
V        = vendor
B        = build

SRC_asm  = $(V)/blake3_avx2_x86-64_unix.S $(V)/blake3_sse41_x86-64_unix.S $(V)/blake3_portable.c
SRC_intr = $(V)/blake3_avx2.c $(V)/blake3_sse41.c $(V)/blake3_portable.c
SRC_port = $(V)/blake3_portable.c
SRC_disp = $(V)/blake3_avx512_x86-64_unix.S $(V)/blake3_avx2_x86-64_unix.S \
           $(V)/blake3_sse41_x86-64_unix.S $(V)/blake3_sse2_x86-64_unix.S
DEF_asm  = -DB3_FLAVOUR='" asm"'
DEF_intr = -DB3_FLAVOUR='" intrinsics"'
DEF_port = -DB3_PORTABLE
DEF_disp = -DB3_DISPATCH
# disp is the distributable build: baseline x86-64, kernels picked at start-up
OPT_disp = -O3

FLAVOURS = asm intr port disp
PROGS    = $(foreach f,$(FLAVOURS),$(B)/b3prove-$(f) $(B)/b3speed-$(f) $(B)/b3min-$(f))

all: $(PROGS)

.SECONDEXPANSION:

# one object per flavour: our layer plus its kernels
$(B)/kern-%.o: $$(SRC_$$*) | $(B)
	$(CC) $(if $(OPT_$*),$(VFLAGS:$(OPT)=$(OPT_$*)),$(VFLAGS)) $(RENAME) -r -nostdlib -o $@ $(SRC_$*)

$(B)/b3tree-%.o: b3tree.c b3tree.h | $(B)
	$(CC) $(if $(OPT_$*),$(CFLAGS:$(OPT)=$(OPT_$*)),$(CFLAGS)) $(RENAME) $(DEF_$*) -c -o $@ $<

$(B)/b3prove-%: prove.c $(B)/b3tree-%.o $(B)/kern-%.o b3tree.h
	$(CC) $(if $(OPT_$*),$(CFLAGS:$(OPT)=$(OPT_$*)),$(CFLAGS)) -o $@ prove.c $(B)/b3tree-$*.o $(B)/kern-$*.o -lblake3

$(B)/b3speed-%: speed.c $(B)/b3tree-%.o $(B)/kern-%.o b3tree.h
	$(CC) $(if $(OPT_$*),$(CFLAGS:$(OPT)=$(OPT_$*)),$(CFLAGS)) -o $@ speed.c $(B)/b3tree-$*.o $(B)/kern-$*.o -lblake3

# the size probe: no libblake3, stripped
$(B)/b3min-%: b3min.c $(B)/b3tree-%.o $(B)/kern-%.o b3tree.h
	$(CC) $(if $(OPT_$*),$(CFLAGS:$(OPT)=$(OPT_$*)),$(CFLAGS)) -s -o $@ b3min.c $(B)/b3tree-$*.o $(B)/kern-$*.o

$(B):
	mkdir -p $@

clean:
	rm -rf $(B)

# keep the per-flavour objects: ../pool links them
.SECONDARY:

.PHONY: all clean
