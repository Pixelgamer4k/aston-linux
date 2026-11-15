#!/bin/bash
# Put Steam back into its first-run (welcome / language / network / sign-in)
# state. Only Steam's "first run done" and "remembered login" markers change;
# everything replaced is kept in a dated backup folder.
set -e
H=/home/steamos
B=$H/.aston-firstrun-backup-$(date +%Y%m%d-%H%M%S)
systemctl stop aston-session
sleep 2
pkill -u steamos -x steam || true
pkill -u steamos steamwebhelper || true
sleep 2
mkdir -p "$B"
cp -a $H/.steam/registry.vdf "$B/registry.vdf"
cp -a $H/.local/share/Steam/config/config.vdf "$B/config.vdf"
for f in config/loginusers.vdf local.vdf; do
    [ -e "$H/.local/share/Steam/$f" ] && mv "$H/.local/share/Steam/$f" "$B/$(basename $f)"
done
sed -i -E '/"(CompletedOOBE|CompletedOOBEStage1|AutoLoginUser|RememberPassword)"/d' $H/.steam/registry.vdf
chown -R steamos:steamos "$B"
echo "backup: $B"
grep -c OOBE $H/.steam/registry.vdf || true
echo game > /etc/aston/session
rm -f /etc/sddm.conf.d/zzt-steamos-temp-login.conf
systemctl start --no-block aston-session
