#!/bin/sh
# Does `restore -r` remove files deleted between a level 0 and a level 1?
# And do user xattrs and POSIX ACLs survive a dump/restore round trip?
#
# Source: ~/tmp/system/SOLUTIONS.md §2026-09-13, extended with the xattr/ACL
# checks the Diavolo brief (PART 4 gap 11, PART 12) asks for. Isolated: two
# 64 MiB loop filesystems and a private dumpdates, so the real set is untouched.
set -e

W=${W:-/var/tmp/dump-deletion-test}
DUMP=/usr/local/sbin/dump
RESTORE=/usr/local/sbin/restore

cleanup() {
	umount "$W/src" 2>/dev/null || true
	umount "$W/dst" 2>/dev/null || true
}
trap cleanup EXIT

for t in setfattr getfattr setfacl getfacl; do
	command -v $t >/dev/null || { echo "missing $t: xbps-install attr-progs acl-progs"; exit 1; }
done

# A previous run may have left these mounted; never rm -rf as root across a mount.
umount "$W/src" "$W/dst" 2>/dev/null || true
if command grep -a -q " $W/" /proc/mounts; then echo "still mounted under $W; aborting"; exit 1; fi
rm -rf "$W"; mkdir -p "$W/src" "$W/dst"
cd "$W"
: > dumpdates

for img in src dst; do
	dd if=/dev/zero of=$img.img bs=1M count=64 status=none
	mke2fs -q -t ext4 -F -L dtest-$img $img.img
	mount -o loop $img.img $img
done

# ---- populate, then level 0 -------------------------------------------------
mkdir -p src/keep src/goesaway
echo original > src/plain-deleted
echo original > src/replaced
echo stays    > src/keep/survivor
echo bye      > src/goesaway/child
echo linked   > src/link-a
ln src/link-a src/link-b
echo attrs    > src/with-xattr
setfattr -n user.diavolo -v irreplaceable src/with-xattr
setfacl -m u:nobody:r src/with-xattr
sync; mount -o remount,ro "$W/src"

$DUMP -0u -D "$W/dumpdates" -f "$W/L0.dump" "$W/src"

# ---- mutate, then level 1 --------------------------------------------------
mount -o remount,rw "$W/src"
rm     src/plain-deleted          # plain file deleted
rm -r  src/goesaway               # whole directory removed
rm     src/replaced               # deleted AND recreated, different content
echo NEW > src/replaced
rm     src/link-b                 # one link of a hardlinked pair
sync; mount -o remount,ro "$W/src"

$DUMP -1u -D "$W/dumpdates" -f "$W/L1.dump" "$W/src"

# ---- restore the chain exactly as a bare-metal restore would --------------
cd "$W/dst"
$RESTORE -rf "$W/L0.dump"
$RESTORE -rf "$W/L1.dump"
rm -f restoresymtable

# ---- verdict ---------------------------------------------------------------
echo
echo "================ RESULT ================"
fail=0
chk() { # chk <description> <present|absent> <path>
	if [ "$2" = absent ]; then
		if [ -e "$3" ]; then echo "FAIL  $1 — still present: $3"; fail=1
		else echo "ok    $1"; fi
	else
		if [ -e "$3" ]; then echo "ok    $1"
		else echo "FAIL  $1 — missing: $3"; fail=1; fi
	fi
}
chk "deleted plain file removed"      absent  "$W/dst/plain-deleted"
chk "removed directory gone"          absent  "$W/dst/goesaway"
chk "surviving file kept"             present "$W/dst/keep/survivor"
chk "recreated file kept"             present "$W/dst/replaced"
chk "removed hardlink gone"           absent  "$W/dst/link-b"
chk "kept hardlink present"           present "$W/dst/link-a"

printf 'replaced content: '; cat "$W/dst/replaced"
printf 'link-a nlink:     '; stat -c %h "$W/dst/link-a"

xa=$(getfattr --only-values -n user.diavolo "$W/dst/with-xattr" 2>/dev/null || true)
if [ "$xa" = irreplaceable ]; then echo "ok    user xattr round-tripped"
else echo "FAIL  user xattr lost (got '$xa')"; fail=1; fi
if getfacl -p "$W/dst/with-xattr" 2>/dev/null | command grep -a -q '^user:nobody:r--'; then
	echo "ok    POSIX ACL round-tripped"
else echo "FAIL  POSIX ACL lost"; fail=1; fi

echo
echo "--- full tree as restored ---"
find "$W/dst" -mindepth 1 -not -path "*/lost+found*" | sed "s|^$W/dst|.|" | sort

echo
[ $fail -eq 0 ] && echo "VERDICT: restore -r DOES replay deletions, and xattrs/ACLs survive." \
                || echo "VERDICT: at least one check FAILED — see above."
echo "Workspace left at $W (rm -rf it when done)."
