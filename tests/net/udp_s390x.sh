#!/usr/bin/env bash
# qemu-user copies the payload of UDP_SEGMENT/GRO control messages verbatim
# between guest and host. Run the UDP suite against a big-endian Linux kernel.
set -euo pipefail
binary=$(realpath "$1")
sysroot=$(realpath "$2")
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
mkdir -p generic overlay/pace-lib
images=https://deb.debian.org/debian/dists/trixie/main/installer-s390x/current/images
for path in SHA256SUMS generic/kernel.debian generic/initrd.debian; do
    curl --retry 3 -fsSL "$images/$path" -o "$path"
done
grep -E '  \./generic/(kernel|initrd)\.debian$' SHA256SUMS | sha256sum -c -
cp "$binary" overlay/pace-udp
cp -L "$sysroot/lib/"{ld64.so.1,libc.so.6,libm.so.6,libstdc++.so.6,libgcc_s.so.1} overlay/pace-lib/
cat > overlay/pace-init <<'EOF'
#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc && mount -t sysfs sysfs /sys && ip link set lo up &&
    /pace-lib/ld64.so.1 --library-path /pace-lib /pace-udp && echo PACE_UDP_SUITE_PASSED
poweroff -f
EOF
chmod +x overlay/pace-init
(cd overlay && find . -print0 | cpio --null -o -H newc --owner=0:0) | gzip >> generic/initrd.debian
# A guest kernel panic or a failed test may still give QEMU exit status zero.
timeout 100 qemu-system-s390x -machine s390-ccw-virtio -cpu max -m 512M -nic none -nographic -no-reboot \
    -kernel generic/kernel.debian -initrd generic/initrd.debian -append 'rdinit=/pace-init panic=-1' | tee console.log
grep -q '^PACE_UDP_SUITE_PASSED' console.log
