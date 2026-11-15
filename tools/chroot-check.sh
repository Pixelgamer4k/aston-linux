#!/system/bin/sh
set -eu
mkdir -p /data/debian/proc /data/debian/sys /data/debian/dev /data/debian/dev/pts /data/debian/tmp /data/debian/run
grep -q " /data/debian/proc " /proc/mounts || mount -t proc proc /data/debian/proc
grep -q " /data/debian/sys " /proc/mounts || mount -t sysfs sysfs /data/debian/sys
grep -q " /data/debian/dev " /proc/mounts || mount -o bind /dev /data/debian/dev
grep -q " /data/debian/dev/pts " /proc/mounts || mount -o bind /dev/pts /data/debian/dev/pts
chroot /data/debian /usr/bin/bash -lc '
echo "== OS =="
cat /etc/os-release
echo "== KERNEL =="
uname -a
echo "== BLOCK =="
ls -l /dev/block/by-name/userdata /dev/block/by-name/super /dev/block/by-name/boot_b /dev/block/by-name/vendor_boot_b
echo "== DRM =="
ls -l /sys/class/drm
echo "== INPUT =="
ls -l /dev/input
echo "== INPUT DEVICES =="
cat /proc/bus/input/devices
echo "== NET =="
ip -br link
echo "== MODULES =="
lsmod | grep -E "msm_drm|ufs_qcom|dwc3|tcm|syna|wlan|audio|snd" || true
echo "== POWER =="
ls /sys/class/power_supply
echo "== UFS =="
ls -d /sys/devices/platform/soc/1d84000.ufshc /sys/class/scsi_device 2>/dev/null || true
echo "== SSH =="
test -x /usr/sbin/sshd && echo sshd_present
test -x /usr/sbin/init && echo init_present
'
