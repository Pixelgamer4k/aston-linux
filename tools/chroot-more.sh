#!/system/bin/sh
set -eu
grep -q " /data/debian/proc " /proc/mounts || mount -t proc proc /data/debian/proc
grep -q " /data/debian/sys " /proc/mounts || mount -t sysfs sysfs /data/debian/sys
grep -q " /data/debian/dev " /proc/mounts || mount -o bind /dev /data/debian/dev
printf 'nameserver 1.1.1.1\n' > /data/debian/etc/resolv.conf
chroot /data/debian /usr/bin/bash /root/more-check.sh
