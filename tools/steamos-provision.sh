#!/bin/sh
# Adapt the official SteamOS ARM tree in /steamos to the OnePlus 12R.
# Run as root from the Debian system on the same partition. It copies our
# device layer (vendor modules, firmware, GPU driver, bring-up services and
# tools) into SteamOS and disables what only applies to Valve's own hardware.
# Safe to run again.
set -e
R=/steamos
[ -x "$R/usr/lib/systemd/systemd" ] || { echo "$R is not a system tree"; exit 1; }
E=$R/etc
U=$E/systemd/system
mkdir -p "$U" "$E/systemd/user" "$R/usr/local/sbin" "$R/usr/local/bin" "$R/usr/local/lib" "$E/aston"

mask() { for u in "$@"; do ln -sfn /dev/null "$U/$u"; done; }
umask_user() { for u in "$@"; do ln -sfn /dev/null "$E/systemd/user/$u"; done; }
enable() { mkdir -p "$U/$2.wants"; ln -sfn "/etc/systemd/system/$1" "$U/$2.wants/$1"; }

echo "== identity"
# The Steam client refuses to start its console interface on the "vr" variant.
for f in "$R/etc/os-release" "$R/usr/lib/os-release"; do
    [ -f "$f" ] && [ ! -L "$f" ] && sed -i 's/^VARIANT_ID=.*/VARIANT_ID="steamdeck"/' "$f"
done
echo aston > "$E/hostname"
ln -sfn "/usr/share/zoneinfo/${ASTON_TZ:-UTC}" "$E/localtime"

echo "== storage layout"
# One ext4 partition holds everything; SteamOS's partition sets do not exist here.
printf '# aston: single ext4 partition, no SteamOS partition sets\n' > "$E/fstab"
mask etc.mount home.mount efi.mount esp.mount persist.mount steamos-offload.target \
     systemd-growfs-root.service 'systemd-growfs@.service' systemd-repart.service
rm -rf "$U/steamos-offload.target.wants"
mkdir -p "$R/home/steamos"
chown 1000:1000 "$R/home/steamos"

echo "== Valve-hardware services off"
mask deckard-audio-setup.service deckard-boot-images.service deckard-charger.service \
     deckard-fan-control.service deckard-fpga.service deckard-fpga-resume.service \
     deckard-led-control.service deckard-power-monitor.service deckard-typec-logger.service \
     steamvr-program-ble.service steamvr-v4l2loopback.service steamvr-set-kernel-thread-priorities.service \
     set-wifi-mac-address.service set-bluetooth-mac-address.service \
     usb-gadget.target usb-gadget.service usb-gadget-init.service adbd.service adbd-pre.service adbd-post.service \
     atomupd.service rauc.service steamos-boot.service setubootenv.service \
     dsp_service.service softapmanager.service iris-driver-rebind.service gpu-trace.service \
     steam-video-codec-setup.service
umask_user steamvr.service steamvr-logs.service steamvr-proxmicmute.service steamvr-v4l2cam.service \
     steamvr-nested-desktop.service
# No virtual terminals in this kernel: the login manager and seatd cannot run.
# Our own session unit (below) starts the session through logind instead.
mask sddm.service seatd.service
# The kernel has no nftables; firewalld would only break networking.
mask firewalld.service
# Wi-Fi goes through wpa_supplicant, as it did under Debian.
mask iwd.service

echo "== vendor modules and firmware"
mkdir -p "$R/usr/lib/modules"
rm -rf "$R/usr/lib/modules/vendor_dlkm"
cp -a /usr/lib/modules/vendor_dlkm "$R/usr/lib/modules/vendor_dlkm"
mkdir -p "$R/usr/lib/firmware" "$R/firmware"
( cd /usr/lib/firmware && for f in a740_* gen70e00_* gmu_gen70* *.jsn vpu30_4v.mbn aston-odm tp wlan qca; do
      [ -e "$f" ] && cp -an "$f" "$R/usr/lib/firmware/" ; done; true )
# SteamOS ships its own WCN7850 Bluetooth firmware for Valve's board; this phone needs its own files.
cp -a /usr/lib/firmware/qca/hmt* "$R/usr/lib/firmware/qca/"

echo "== device tools and services"
for f in aston-adsp aston-gpu aston-touch aston-wifi aston-partlabels aston-btfw aston-btpower \
         aston-boot-ok aston-slotflag aston-modload aston-lpmap aston-powerd busybox reboot2 rmtfs; do
    cp -a "/usr/local/sbin/$f" "$R/usr/local/sbin/"
done
ln -sfn busybox "$R/usr/local/sbin/mdev"
cp -a /usr/local/bin/aston /usr/local/bin/vdectest "$R/usr/local/bin/" 2>/dev/null || true
# Modem helper daemons are not part of SteamOS; bring Debian's along with their one library.
cp -a /usr/bin/qrtr-ns /usr/bin/qrtr-lookup /usr/bin/tqftpserv "$R/usr/local/bin/"
cp -aL /lib/aarch64-linux-gnu/libqrtr.so.1 "$R/usr/local/lib/"
echo /usr/local/lib > "$E/ld.so.conf.d/00-aston-local.conf"
sed -i 's|M=/lib/modules/vendor_dlkm|M=/usr/lib/modules/vendor_dlkm|' "$R/usr/local/sbin/aston-wifi"
# With devtmpfs the kernel makes the nodes; a busybox rescan would only reset their permissions.
sed -i "/mdev -s/d" "$R/usr/local/sbin/aston-touch" "$R/usr/local/sbin/aston-gpu"
cp -a /etc/aston/power.conf "$E/aston/" 2>/dev/null || true
cp -a /etc/aston/bdaddr "$E/aston/" 2>/dev/null || true

for u in aston-adsp aston-gpu aston-touch aston-wifi aston-bluetooth aston-powerd aston-boot-ok; do
    cp -a "/etc/systemd/system/$u.service" "$U/"
done
sed -i 's/^After=aston-mdev.service systemd-rfkill.service/After=systemd-rfkill.service/' "$U/aston-bluetooth.service"
cat > "$U/rmtfs.service" <<'EOF'
[Unit]
Description=Qualcomm remotefs service
Before=NetworkManager.service
[Service]
ExecStartPre=/usr/local/sbin/aston-partlabels
ExecStart=/usr/local/sbin/rmtfs -r -P -s
Restart=always
RestartSec=1
[Install]
WantedBy=multi-user.target
EOF
cat > "$U/qrtr-ns.service" <<'EOF'
[Unit]
Description=QIPCRTR Name Service
[Service]
ExecStart=/usr/local/bin/qrtr-ns -f 1
Restart=always
[Install]
WantedBy=multi-user.target
EOF
cat > "$U/tqftpserv.service" <<'EOF'
[Unit]
Description=QRTR TFTP service
[Service]
ExecStart=/usr/local/bin/tqftpserv
Restart=always
[Install]
WantedBy=multi-user.target
EOF
enable aston-adsp.service sysinit.target
for u in aston-gpu aston-touch aston-wifi aston-bluetooth rmtfs qrtr-ns tqftpserv; do enable "$u.service" multi-user.target; done
for u in aston-powerd aston-boot-ok; do enable "$u.service" graphical.target; done
for u in upower power-profiles-daemon; do
    mkdir -p "$U/$u.service.d"
    printf '[Service]\nPrivateUsers=no\n' > "$U/$u.service.d/aston.conf"
done

echo "== udev, tmpfiles, polkit"
mkdir -p "$E/udev/hwdb.d" "$E/udev/rules.d" "$E/tmpfiles.d" "$E/polkit-1/rules.d"
cp -a /etc/udev/hwdb.d/61-aston-touch.hwdb "$E/udev/hwdb.d/"
cp -a /etc/udev/rules.d/61-aston-bluetooth.rules "$E/udev/rules.d/"
cat > "$E/udev/rules.d/60-aston-devices.rules" <<'EOF'
# Device nodes now come from devtmpfs; these are the permissions the GPU
# driver, the video decoder and input tools need.
KERNEL=="kgsl-3d0", MODE="0666"
SUBSYSTEM=="dma_heap", KERNEL=="system", MODE="0666"
SUBSYSTEM=="dma_heap", KERNEL=="qcom,system", MODE="0666"
KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
KERNEL=="btpower", MODE="0660"
EOF
cp -a /etc/tmpfiles.d/aston-display.conf /etc/tmpfiles.d/aston-leds.conf "$E/tmpfiles.d/"
sed 's/isInGroup("sudo")/isInGroup("wheel")/' /etc/polkit-1/rules.d/50-aston-device-settings.rules > "$E/polkit-1/rules.d/50-aston-device-settings.rules"

echo "== network"
mkdir -p "$E/NetworkManager/conf.d" "$E/NetworkManager/system-connections"
cp -a /etc/NetworkManager/conf.d/aston.conf "$E/NetworkManager/conf.d/"
printf '[device]\nwifi.backend=wpa_supplicant\n' > "$E/NetworkManager/conf.d/10-aston-wifi-backend.conf"
# Steam asks SteamOS to switch Wi-Fi to iwd by rewriting this file, which takes
# Wi-Fi down here. Pin it to wpa_supplicant and make it read-only.
chattr -i "$E/NetworkManager/conf.d/99-valve-wifi-backend.conf" 2>/dev/null || true
printf '[device]\nwifi.backend=wpa_supplicant\n' > "$E/NetworkManager/conf.d/99-valve-wifi-backend.conf"
chattr +i "$E/NetworkManager/conf.d/99-valve-wifi-backend.conf"
cp -a /etc/NetworkManager/system-connections/. "$E/NetworkManager/system-connections/"
chmod 600 "$E/NetworkManager/system-connections/"* 2>/dev/null || true

echo "== GPU driver"
# Valve's Mesa talks to the mainline msm DRM driver. This kernel exposes the
# Adreno through KGSL, so keep our KGSL build beside it and make it the default.
rm -rf "$R/opt/mesa-kgsl"
mkdir -p "$R/opt"
cp -a /opt/mesa-kgsl "$R/opt/mesa-kgsl"
echo /opt/mesa-kgsl/usr/lib/aarch64-linux-gnu > "$E/ld.so.conf.d/00-aston-mesa.conf"
mkdir -p "$E/vulkan/icd.d" "$R/usr/share/vulkan/icd.d.valve"
cp -a /etc/vulkan/icd.d/turnip_kgsl.json "$E/vulkan/icd.d/"
for j in "$R"/usr/share/vulkan/icd.d/*.json; do [ -e "$j" ] && mv "$j" "$R/usr/share/vulkan/icd.d.valve/"; done
cp -a /etc/drirc "$E/drirc"
cp -a /etc/aston/gpu.env "$E/aston/gpu.env"
grep -q GBM_BACKENDS_PATH "$E/environment" 2>/dev/null || cat /etc/aston/gpu.env >> "$E/environment"
chroot "$R" /usr/bin/ldconfig

echo "== remote access"
mkdir -p "$R/root/.ssh" "$R/home/steamos/.ssh" "$E/ssh/sshd_config.d"
cp -a /root/.ssh/authorized_keys "$R/root/.ssh/"
cp -a /root/.ssh/authorized_keys "$R/home/steamos/.ssh/"
chown -R 1000:1000 "$R/home/steamos/.ssh"; chmod 700 "$R/root/.ssh" "$R/home/steamos/.ssh"
printf '# aston-linux\nPermitRootLogin prohibit-password\nPasswordAuthentication no\nPermitEmptyPasswords no\n' > "$E/ssh/sshd_config.d/10-aston.conf"
grep -q 'aston-linux' "$E/ssh/sshd_config" || printf '\n# aston-linux: settings are in sshd_config.d/10-aston.conf\n' >> "$E/ssh/sshd_config"
mount --bind /dev "$R/dev"
chroot "$R" /usr/bin/ssh-keygen -A >/dev/null || true
umount "$R/dev"
ln -sfn /usr/lib/systemd/system/sshd.service "$U/multi-user.target.wants/sshd.service"
printf '%%wheel ALL=(ALL) NOPASSWD:ALL\n' > "$E/sudoers.d/zz-aston"; chmod 440 "$E/sudoers.d/zz-aston"

echo "== session"
cat > "$R/usr/local/bin/aston-session-start" <<'EOF'
#!/bin/sh
# Start the session chosen in /etc/aston/session.
#   desktop  SteamOS Desktop Mode (KDE Plasma)
mode=$(cat /etc/aston/session 2>/dev/null)
case "$mode" in
    *) exec /usr/lib/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland ;;
esac
EOF
chmod 755 "$R/usr/local/bin/aston-session-start"
echo desktop > "$E/aston/session"
cat > "$U/aston-session.service" <<'EOF'
[Unit]
Description=SteamOS session on the phone panel
After=systemd-user-sessions.service systemd-logind.service dbus.service systemd-tmpfiles-setup.service aston-gpu.service aston-touch.service
Wants=aston-gpu.service aston-touch.service

[Service]
User=steamos
PAMName=login
WorkingDirectory=~
EnvironmentFile=/etc/aston/gpu.env
Environment=XDG_SESSION_TYPE=wayland XDG_SESSION_CLASS=user XDG_SEAT=seat0 XDG_CURRENT_DESKTOP=KDE
ExecStart=/usr/local/bin/aston-session-start
Restart=on-failure
RestartSec=5
ExecStopPost=+/usr/bin/loginctl terminate-user steamos

[Install]
WantedBy=graphical.target
EOF
enable aston-session.service graphical.target

echo "== done"
du -sh "$R" | cut -f1
