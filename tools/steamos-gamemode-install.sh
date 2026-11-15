#!/bin/bash
# Gaming Mode on SteamOS for the OnePlus 12R: Gamescope owning the panel with
# the Steam interface inside it, and Steam's Quick Access controls wired to this
# phone. Applied on top of steamos-provision.sh; safe to run again.
#
# Run on the phone as root, from a directory holding these files (they live in
# tools/ and out/ of the project):
#   aston-gamescope-session aston-gamescope-steam aston-session-start
#   steamos-session-select gamescope.conf aston aston-powerd aston-steamosd
#   aston-steamosd.service com.aston.SteamOS.conf steamos-manager-remote-aston.toml
#   libaston-drm-informats.so libaston-nohwmon.so
# and with the newer Mesa build unpacked at /opt/mesa-kgsl2.
set -e
cd "$(dirname "$0")"

# --- Mesa: GL and Vulkan must come from the same build -----------------------
# (the older GL library with the newer Vulkan driver makes buffers Gamescope's
# X server cannot import, which kills Steam's web interface)
M=/opt/mesa-kgsl2/usr
[ -d "$M/lib/dri" ] || { echo "missing $M"; exit 1; }
cat > /etc/aston/gpu.env <<EOF
GBM_BACKENDS_PATH=$M/lib/gbm
LIBGL_DRIVERS_PATH=$M/lib/dri
__EGL_VENDOR_LIBRARY_FILENAMES=$M/share/glvnd/egl_vendor.d/50_mesa.json
VK_DRIVER_FILES=/etc/vulkan/icd.d/turnip_kgsl.json
MESA_LOADER_DRIVER_OVERRIDE=zink
EOF
echo "$M/lib" > /etc/ld.so.conf.d/00-aston-mesa.conf
sed -i "s|/opt/mesa-kgsl/usr/lib/aarch64-linux-gnu|$M/lib|g; s|/opt/mesa-kgsl/usr|$M|g" /etc/environment
ldconfig

# --- Gamescope session -------------------------------------------------------
install -m 755 aston-gamescope-session aston-gamescope-steam aston-session-start aston /usr/local/bin/
install -m 644 libaston-drm-informats.so libaston-nohwmon.so /usr/local/lib/
[ -f /etc/aston/gamescope.conf ] || install -m 644 gamescope.conf /etc/aston/gamescope.conf
# Gamescope without file capabilities: a privileged program ignores LD_PRELOAD.
install -m 755 /usr/bin/gamescope /usr/local/bin/gamescope-drm
mkdir -p /var/empty

# Switching between Gaming Mode and Desktop Mode.
[ -f /usr/bin/steamos-session-select.valve ] || cp -a /usr/bin/steamos-session-select /usr/bin/steamos-session-select.valve
install -m 755 steamos-session-select /usr/bin/steamos-session-select
sed -i "s/^Restart=on-failure/Restart=always/" /etc/systemd/system/aston-session.service
# Steam belongs to Gaming Mode now, not to the Plasma session.
rm -f /etc/systemd/user/plasma-workspace.target.wants/steam.service

# --- Steam Quick Access controls ---------------------------------------------
install -m 755 aston-powerd aston-steamosd /usr/local/sbin/
install -m 644 aston-steamosd.service /etc/systemd/system/
mkdir -p /etc/dbus-1/system.d /etc/steamos-manager/remotes.d
install -m 644 com.aston.SteamOS.conf /etc/dbus-1/system.d/
install -m 644 steamos-manager-remote-aston.toml /etc/steamos-manager/remotes.d/aston.toml
systemctl daemon-reload
systemctl enable aston-steamosd
echo "installed; start with: aston mode game"

# Volume Up + Volume Down: Quick Access menu
install -m 755 aston-keysd /usr/local/sbin/
install -m 644 aston-keysd.service /etc/systemd/system/
systemctl enable aston-keysd

# Steam's "Switch to Desktop" leaves a one-time session choice for SDDM;
# aston-session-start reads it. It must not outlive a reboot.
echo "r /etc/sddm.conf.d/zzt-steamos-temp-login.conf" > /etc/tmpfiles.d/aston-session-once.conf
# Desktop Mode: let Valve's autostart run Steam in the background, as on a
# Steam Deck, so the controller drives the mouse and keyboard there.
rm -f /home/steamos/.config/autostart/steam.desktop
systemctl mask serial-getty@hvc0.service
