#!/bin/sh
# accbench harness: builds the test filesystems, proves every method and
# language reads identical bytes, then times them. Needs root (losetup,
# mount, drop_caches, open_by_handle_at, bulkstat, perf counters).
#
# Build first AS YOURSELF (cargo needs your ~/.cargo):  make -C bench/access
# Then:                        doas bench/access/run.sh all
# Quick smoke test (minutes):  doas env SCALE=0.02 REPS=1 REAL_SRC=/usr/share/terminfo bench/access/run.sh all
#   (SCALE does not shrink the real set; REAL_SRC does.)
#
# Steps: prepare (make images) -> verify (digest gate) -> bench (timed) -> trace
# (strace -c). Afterwards, as yourself: python3 bench/access/report.py
#
# Knobs (environment): W SCALE REPS REGIMES EXT4_SETS XFS_SETS TRACE_SETS
# REAL_SRC CPU FORCE METHODS. Defaults below; see README.md.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
W=${W:-/var/tmp/accbench}
SCALE=${SCALE:-1}
REPS=${REPS:-3}
REGIMES=${REGIMES:-"cold warm warm-hash"}
EXT4_SETS=${EXT4_SETS-"huge-fresh-random huge-aged-random tiny-fresh-random tiny-aged-random small-fresh-random small-aged-random mixed-fresh-random mixed-aged-random mixed-aged-text real-fresh real-aged"}
XFS_SETS=${XFS_SETS-"tiny-aged-random small-aged-random mixed-aged-random real-aged"}
TRACE_SETS=${TRACE_SETS-"small-aged-random mixed-aged-random"}
REAL_SRC=${REAL_SRC:-/usr/share}
CPU=${CPU:-2}
FORCE=${FORCE:-0}
METHODS=${METHODS:-}
IMPLS="$HERE/c/build/accbench-gcc $HERE/c/build/accbench-clang $HERE/rust/target/release/accbench"
MNT=$W/mnt
LOOP=

die() { echo "run.sh: $*" >&2; exit 1; }
log() { echo "[$(date +%H:%M:%S)] $*"; }

# Every method that applies to a filesystem, narrowed to $METHODS when that is set.
methods_for() {
	case $1 in ext4) all="vfs handle e2fs raw rawsort" ;; xfs) all="vfs handle bulkstat" ;; esac
	[ -z "$METHODS" ] && { echo "$all"; return; }
	for m in $all; do case " $METHODS " in *" $m "*) printf '%s ' "$m" ;; esac; done; echo
}
target_for() { case $1 in e2fs|raw|rawsort) echo "$LOOP" ;; *) echo "$MNT" ;; esac; }
impl_name() { case $1 in *accbench-gcc) echo c-gcc ;; *accbench-clang) echo c-clang ;; *) echo rust ;; esac; }

detach() {
	if mountpoint -q "$MNT" 2>/dev/null; then umount "$MNT"; fi
	if [ -n "$LOOP" ]; then losetup -d "$LOOP" 2>/dev/null || true; LOOP=; fi
}
cleanup() {
	detach
	if [ -n "${DOAS_USER:-${SUDO_USER:-}}" ] && [ -d "$W/results" ]; then
		chown -R "${DOAS_USER:-$SUDO_USER}" "$W/results"
	fi
}
trap cleanup EXIT INT TERM

# attach IMG ro|rw: loop device (direct I/O when read-only, so the backing
# file's page cache can't serve reads twice) and mount it on $MNT.
attach() {
	if [ "$2" = ro ]; then
		LOOP=$(losetup --find --show --direct-io=on --read-only "$1")
		dio=$(cat "/sys/block/${LOOP#/dev/}/loop/dio" 2>/dev/null || echo 0)
		[ "$dio" = 1 ] || die "$LOOP did not get direct I/O (dio=$dio); cold runs would not be cold"
		mount -o ro "$LOOP" "$MNT"
	else
		LOOP=$(losetup --find --show "$1")
		mount "$LOOP" "$MNT"
	fi
}

# Image size in MiB for a set, generous enough for the aged layout's fillers.
image_mib() {
	kind=${1%%-*}
	case $kind in
	huge)  base=$(awk "BEGIN{print int(2048*$SCALE*2.2)}") ;;
	tiny)  base=$(awk "BEGIN{print int(400*$SCALE*2)}") ;;
	small) base=$(awk "BEGIN{print int(1000*$SCALE*1.6)}") ;;
	mixed) base=$(awk "BEGIN{print int(1536*$SCALE*1.6)+256}") ;;
	real)  base=$(du -sm "$REAL_SRC" | awk '{print int($1*1.6)}') ;;
	esac
	echo $((base + 768))
}

preflight() {
	[ "$(id -u)" = 0 ] || die "needs root: doas $0 $*"
	for b in $IMPLS; do [ -x "$b" ] || die "missing $b; build first as yourself: make -C $HERE"; done
	for t in losetup mkfs.ext4 mkfs.xfs e2fsck xfs_db python3 taskset shuf strace; do
		command -v $t >/dev/null || die "missing $t"
	done
	mkdir -p "$W/img" "$W/dig" "$W/results" "$MNT"
	if grep -q " $MNT " /proc/mounts; then die "$MNT is still mounted; unmount it first"; fi
}

# ---- prepare ----

# The parameters an image was built with. An image built with other values
# (a smoke run's SCALE, another REAL_SRC) is stale and is never timed.
params() { echo "params: scale=$SCALE real_src=$REAL_SRC"; }
current() { [ -f "$1.info" ] && [ "$(head -1 "$1.info")" = "$(params)" ]; }

prepare_one() {
	fs=$1 set=$2 img=$W/img/$1-$2.img
	if [ -f "$img" ] && current "$img" && [ "$FORCE" != 1 ]; then log "have $fs $set"; return; fi
	[ -f "$img.info" ] && log "rebuilding $fs $set: built with $(head -1 "$img.info" | sed 's/^params: //'), want $(params | sed 's/^params: //')"
	kind=$(echo "$set" | cut -d- -f1) layout=$(echo "$set" | cut -d- -f2) content=$(echo "$set" | cut -d- -f3)
	[ -n "$content" ] || content=random
	mib=$(image_mib "$set")
	log "prepare $fs $set (${mib} MiB image)"
	rm -f "$img" "$img.info" "$img.verified"
	truncate -s "${mib}M" "$img"
	case $fs in
	ext4)
		iopt=; [ "$kind" = tiny ] && iopt="-i 4096"
		# shellcheck disable=SC2086
		mkfs.ext4 -q -F -O ^inline_data $iopt -E lazy_itable_init=0,lazy_journal_init=0 -L accbench "$img" ;;
	xfs) mkfs.xfs -q -f -L accbench "$img" ;;
	esac
	attach "$img" rw
	params > "$img.info.tmp"
	python3 "$HERE/gen/mkset.py" "$kind" "$MNT" --layout "$layout" --content "$content" \
		--scale "$SCALE" --from "$REAL_SRC" >> "$img.info.tmp"
	python3 "$HERE/gen/layout.py" "$MNT" >> "$img.info.tmp"    # did 'aged' really disorder it?
	df -h "$MNT" | tail -1 >> "$img.info.tmp"
	umount "$MNT"
	case $fs in
	ext4) e2fsck -fn "$LOOP" 2>&1 | tail -1 >> "$img.info.tmp" ;;   # "N/M files (x% non-contiguous)"
	xfs)  xfs_db -r -c frag "$LOOP" >> "$img.info.tmp" ;;           # "fragmentation factor"
	esac
	losetup -d "$LOOP"; LOOP=
	mv "$img.info.tmp" "$img.info"
	sed 's/^/    /' "$img.info"
}

# ---- verify: every method x implementation must produce identical digests ----

verify_one() {
	fs=$1 set=$2 img=$W/img/$1-$2.img
	current "$img" || { log "skip verify $fs $set: not prepared with $(params)"; return; }
	attach "$img" ro
	ref= ok=1
	for m in $(methods_for "$fs"); do
		for b in $IMPLS; do
			d=$W/dig/$fs-$set.$(impl_name "$b").$m
			if ! "$b" "$m" "$(target_for "$m")" --digest "$d" > /dev/null; then
				echo "    FAIL: $(impl_name "$b") $m exited non-zero"; ok=0; continue
			fi
			files=$(head -1 "$d" | sed 's/.*files=\([0-9]*\).*/\1/')
			if [ "${files:-0}" -eq 0 ]; then echo "    FAIL: $(impl_name "$b") $m found no files"; ok=0; continue; fi
			if [ -z "$ref" ]; then ref=$d
			elif ! cmp -s "$ref" "$d"; then
				echo "    FAIL: $(impl_name "$b") $m differs from $(basename "$ref")"; diff "$ref" "$d" | head -5; ok=0
			fi
		done
	done
	detach
	if [ $ok = 1 ]; then
		head -1 "$ref" > "$img.verified"
		log "verified $fs $set: $(cat "$img.verified")"
	else
		rm -f "$img.verified"
		log "VERIFY FAILED $fs $set: excluded from timing"
	fi
}

# ---- bench ----

dev_stat() { awk '{print $1, $3}' "/sys/block/${LOOP#/dev/}/stat"; }   # reads completed, sectors read

# After drop_caches, page the programs and their libraries back in, so a cold
# run pays for the filesystem under test, not for faulting in its own code.
CODE=
code_files() {
	for b in $IMPLS; do
		echo "$b"
		ldd "$b" | awk '/=> \//{print $3} /^\t\/.*\(/{print $1}'
	done | sort -u
	command -v perf taskset
}
warm_code() { [ -n "$CODE" ] || CODE=$(code_files); cat $CODE > /dev/null; }

perf_ok=0
bench_run() {  # fs set regime rep impl method
	b=$5 m=$6 hash=
	case $3 in *-hash) hash=--hash ;; esac
	case $3 in cold*) sync; echo 3 > /proc/sys/vm/drop_caches; warm_code ;; esac
	set -- "$@" "$(dev_stat)"
	if [ $perf_ok = 1 ]; then
		line=$(taskset -c "$CPU" perf stat -x, -o "$W/perf.tmp" \
			-e cycles:u,instructions:u,cycles:k,instructions:k -- "$b" "$m" "$(target_for "$m")" $hash)
		pc=$(awk -F, '/cycles:u/{a=$1} /instructions:u/{b=$1} /cycles:k/{c=$1} /instructions:k/{d=$1} END{print a","b","c","d}' "$W/perf.tmp")
	else
		line=$(taskset -c "$CPU" "$b" "$m" "$(target_for "$m")" $hash)
		pc=",,,"
	fi
	after=$(dev_stat)
	before=$7
	dr=$(( ${after% *} - ${before% *} )) ds=$(( ${after#* } - ${before#* } ))
	echo "$1,$2,$3,$4,$line,$dr,$ds,$pc" >> "$R/results.csv"
}

bench_one() {
	fs=$1 set=$2 img=$W/img/$1-$2.img
	current "$img" || { log "skip bench $fs $set: not prepared with $(params)"; return; }
	[ -f "$img.verified" ] || { log "skip bench $fs $set: not verified"; return; }
	attach "$img" ro
	for regime in $REGIMES; do
		case $regime in warm*)   # warm every cache a method uses: file pages and device pages
			for m in $(methods_for "$fs"); do "$HERE/c/build/accbench-gcc" "$m" "$(target_for "$m")" > /dev/null; done ;;
		esac
		rep=1
		while [ $rep -le "$REPS" ]; do
			# every impl x method once per rep, in a fresh random order, so drift can't favour one
			for pair in $(for b in $IMPLS; do for m in $(methods_for "$fs"); do echo "$b:$m"; done; done | shuf); do
				bench_run "$fs" "$set" "$regime" "$rep" "${pair%:*}" "${pair##*:}"
			done
			log "  $fs $set $regime rep $rep/$REPS"
			rep=$((rep + 1))
		done
	done
	detach
}

write_env() {
	{
		echo "date: $(date -Is)"
		echo "kernel: $(uname -r)"
		echo "cmdline: $(cat /proc/cmdline)"
		echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-) (runs pinned to cpu $CPU)"
		echo "governor: $(cat /sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
		echo "gcc: $(gcc --version | head -1)"
		echo "clang: $(clang --version | head -1)"
		echo "rustc: $(rustc --version 2>/dev/null || echo 'not in root PATH')"
		echo "e2fsprogs: $(mke2fs -V 2>&1 | head -1)"
		echo "xfsprogs: $(mkfs.xfs -V)"
		echo "repo: $(git -C "$HERE" rev-parse --short HEAD 2>/dev/null)$(git -C "$HERE" diff --quiet 2>/dev/null || echo ' (dirty)')"
		echo "images on: $(df -h "$W/img" | tail -1)"
		echo "backing device rotational: $(lsblk -no ROTA "$(df --output=source "$W/img" | tail -1)" 2>/dev/null | head -1)"
		echo "scale=$SCALE reps=$REPS regimes=$REGIMES methods=${METHODS:-all}"
		echo "perf counters: $([ $perf_ok = 1 ] && echo yes || echo no)"
		echo "loop devices: direct I/O required and checked on every read-only attach"
		echo "cold runs re-read after drop_caches: $(code_files | tr '\n' ' ')"
	} > "$R/env.txt"
}

# ---- main ----

each() {  # run $1 over every configured fs/set
	for s in $EXT4_SETS; do "$1" ext4 "$s"; done
	for s in $XFS_SETS; do "$1" xfs "$s"; done
}

cmd=${1:-all}
preflight "$@"
case $cmd in
prepare) each prepare_one ;;
verify)  each verify_one ;;
bench|trace|all)
	[ "$cmd" = all ] && { each prepare_one; each verify_one; }
	R=$W/results/$(date +%Y%m%d-%H%M%S)
	mkdir -p "$R"
	ln -sfn "$R" "$W/results/latest"
	if perf stat -x, -o /dev/null -e cycles:u true 2>/dev/null; then perf_ok=1; fi
	write_env
	for f in "$W"/img/*.info; do [ -f "$f" ] && { echo "== $(basename "$f" .img.info)"; cat "$f"; cat "${f%.info}.verified" 2>/dev/null; }; done > "$R/images.txt"
	if [ "$cmd" != trace ]; then
		echo "fs,set,regime,rep,$("$HERE/c/build/accbench-gcc" --header),dev_reads,dev_sectors,cycles_u,instr_u,cycles_k,instr_k" > "$R/results.csv"
		each bench_one
	fi
	if [ "$cmd" != bench ]; then
		mkdir -p "$R/trace"
		for fs in ext4 xfs; do
			for s in $TRACE_SETS; do
				img=$W/img/$fs-$s.img
				current "$img" && [ -f "$img.verified" ] || continue
				attach "$img" ro
				for m in $(methods_for "$fs"); do
					for b in $IMPLS; do
						strace -f -c -o "$R/trace/$fs-$s-$(impl_name "$b")-$m.txt" "$b" "$m" "$(target_for "$m")" > /dev/null
					done
				done
				detach
				log "traced $fs $s"
			done
		done
	fi
	log "results in $R"
	;;
clean)
	detach
	rm -rf "$W/dig"
	for f in "$W"/img/*.img; do [ -f "$f" ] && rm -f "$f" "$f.info" "$f.verified"; done
	log "images removed; results kept in $W/results"
	;;
*) die "usage: $0 [all|prepare|verify|bench|trace|clean]" ;;
esac
