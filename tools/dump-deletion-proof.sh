#!/bin/sh
# Direct evidence that `restore -r` removes files deleted between dumps.
#
# Instead of inferring removal from absence, it traces every unlink, rmdir and
# rename that restore itself performs, and prints them. It also removes the two
# confounds found in dump-deletion-test.sh (docs/design/0003, addendum 2):
#   - L0 and L1 ran in the same second, so L1 re-dumped every file.
#     Here: 2 s gaps, and an abort unless L1 omits the unchanged files.
#   - A deleted file's inode number was reused by a recreated file, so restore
#     renamed it instead of deleting it. Here: the new inode is allocated
#     before any inode is freed.
#
# Root is needed for mke2fs, the loop mount and dump. Workspace: $W.
set -eu

W=${W:-/var/tmp/dump-deletion-proof}
DUMP=/usr/local/sbin/dump
RESTORE=/usr/local/sbin/restore
TRACE=trace=unlink,unlinkat,rmdir,rename,renameat,renameat2

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v strace >/dev/null || { echo "needs strace"; exit 1; }

cleanup() { umount "$W/src" 2>/dev/null || true; }
trap cleanup EXIT
umount "$W/src" 2>/dev/null || true
if grep -a -q " $W/" /proc/mounts; then echo "still mounted under $W; aborting"; exit 1; fi
rm -rf "$W"
mkdir -p "$W/src" "$W/restore-r" "$W/restore-x"
: > "$W/dumpdates"

step() { printf '\n==== %s ====\n' "$*"; }
# inode <TAB> type <TAB> path, lost+found and restore's own bookkeeping omitted
tree() { (cd "$1" && find . -mindepth 1 -not -name restoresymtable -not -path './lost+found*' \
	-printf '%i\t%y\t%p\n' | sort -t "$(printf '\t')" -k3); }
ino_of() { awk -F '\t' -v p="./$2" '$3 == p { print $1 }' "$1"; }
syscalls() { sed 's/^[0-9]* *//' "$1" | grep -a -v 'rstmode\|rstdir\|^---\|^+++' || true; }
dump_at() { # dump_at <level> <file>
	"$DUMP" "-$1u" -D "$W/dumpdates" -f "$W/$2" "$W/src" > "$W/$2.log" 2>&1 ||
		{ cat "$W/$2.log"; exit 1; }
}

step "versions under test"
"$DUMP" 2>&1 | head -1
"$RESTORE" 2>&1 | head -1
strace -V | head -1

dd if=/dev/zero of="$W/src.img" bs=1M count=64 status=none
mke2fs -q -t ext4 -F "$W/src.img"
mount -o loop "$W/src.img" "$W/src"

step "populate the source filesystem"
mkdir "$W/src/DELETED-dir" "$W/src/UNCHANGED-dir"
echo child     > "$W/src/DELETED-dir/child"
echo doomed    > "$W/src/DELETED-file"
echo unchanged > "$W/src/UNCHANGED-dir/file"
echo unchanged > "$W/src/UNCHANGED-file"
echo old       > "$W/src/RECREATED-file"
echo linked    > "$W/src/LINK-kept"
ln "$W/src/LINK-kept" "$W/src/LINK-removed"
sync
tree "$W/src"
sleep 2    # dump dates have 1 s resolution
mount -o remount,ro "$W/src"
dump_at 0 L0.dump
sleep 2

step "mutate: delete a file, a directory and one hardlink; recreate a file"
mount -o remount,rw "$W/src"
echo new > "$W/src/RECREATED-file.tmp"     # new inode allocated before any is freed
mv "$W/src/RECREATED-file.tmp" "$W/src/RECREATED-file"
rm    "$W/src/DELETED-file"
rm -r "$W/src/DELETED-dir"
rm    "$W/src/LINK-removed"
sync
tree "$W/src"
sleep 2
mount -o remount,ro "$W/src"
dump_at 1 L1.dump

step "contents of L1 (a genuine incremental does not list UNCHANGED-*)"
"$RESTORE" -tf "$W/L1.dump" 2>/dev/null | tee "$W/L1.list"
if grep -a -q UNCHANGED "$W/L1.list"; then
	echo "ABORT: L1 contains unchanged files, so it is not a genuine incremental"; exit 1
fi

step "restore -r L0 into restore-r/  (tree: inode, type, path)"
cd "$W/restore-r"
"$RESTORE" -rf "$W/L0.dump" > /dev/null 2>&1
tree . | tee "$W/after-L0.txt"

step "restore -r L1: every unlink/rmdir/rename that restore performs"
strace -f -qq -e "$TRACE" -e signal=none -o "$W/r.trace" "$RESTORE" -rf "$W/L1.dump" > /dev/null 2>&1
syscalls "$W/r.trace"

step "tree after L1 (compare inode numbers with the tree after L0)"
tree . | tee "$W/after-L1.txt"
rm -f restoresymtable

step "control: restore -r L0, then restore -x L1, into restore-x/"
cd "$W/restore-x"
"$RESTORE" -rf "$W/L0.dump" > /dev/null 2>&1
printf '1\nn\n' | strace -f -qq -e "$TRACE" -e signal=none -o "$W/x.trace" \
	timeout 60 "$RESTORE" -xf "$W/L1.dump" > /dev/null 2>&1 || true
echo "syscalls:"; syscalls "$W/x.trace"
tree .
cd "$W"

step "verdict"
fail=0
ok()  { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fail=1; }
t=$W/r.trace
grep -a -q 'unlink("./DELETED-file")' "$t" &&
	ok "restore -r called unlink on DELETED-file" || bad "no unlink of DELETED-file in the trace"
grep -a -q 'unlink("./DELETED-dir/child")' "$t" &&
	ok "restore -r called unlink on DELETED-dir/child" || bad "no unlink of DELETED-dir/child"
if grep -a -q 'rename("./DELETED-dir", "./RSTTMP' "$t" && grep -a -q 'rmdir("./RSTTMP' "$t"; then
	ok "restore -r renamed DELETED-dir aside, then called rmdir on it"
else bad "no rename+rmdir of DELETED-dir"; fi
grep -a -q 'unlink("./LINK-removed")' "$t" &&
	ok "restore -r called unlink on LINK-removed" || bad "no unlink of LINK-removed"
for p in DELETED-file DELETED-dir LINK-removed; do
	[ -e "$W/restore-r/$p" ] && bad "$p still present after restore -r" || ok "$p absent after restore -r"
done
for p in UNCHANGED-file UNCHANGED-dir/file LINK-kept; do
	a=$(ino_of "$W/after-L0.txt" "$p"); b=$(ino_of "$W/after-L1.txt" "$p")
	[ -n "$a" ] && [ "$a" = "$b" ] &&
		ok "$p kept inode $a across the L1 pass (not rebuilt)" || bad "$p inode changed: $a -> $b"
done
for p in UNCHANGED-file UNCHANGED-dir/file; do
	grep -a -q "\"./$p\"" "$t" && bad "restore made a syscall on $p" || ok "restore made no syscall on $p"
done
[ "$(cat "$W/restore-r/RECREATED-file")" = new ] &&
	ok "RECREATED-file has its new content" || bad "RECREATED-file content wrong"
for p in DELETED-file DELETED-dir LINK-removed; do
	[ -e "$W/restore-x/$p" ] && ok "control: restore -x left $p in place" ||
		bad "control: $p missing after restore -x"
done
grep -a -q 'unlink("./DELETED\|unlink("./LINK-removed\|rmdir("./RSTTMP' "$W/x.trace" &&
	bad "control: restore -x made removal syscalls" || ok "control: restore -x made no removal syscalls"

echo
[ $fail -eq 0 ] && echo "VERDICT: restore -r itself removes files deleted between dumps; restore -x does not." \
                || echo "VERDICT: at least one check FAILED; see above."
echo "Traces and trees kept in $W"
