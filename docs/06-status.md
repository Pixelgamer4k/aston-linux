# 6. Feature status

Final state before the phone was reverted to stock.
**Done** = seen working on the device · **Partial** = works with caveats · **Unverified** =
built but never confirmed · **Broken** = tried and failed · **Todo** = not attempted ·
**No** = not realistic on this kernel

## Platform

| Item | Status | Note |
|---|---|---|
| Dual boot with OxygenOS (slot A/B) | Done | `fastboot --set-active` to switch |
| Slot-A "successful" flag maintained | Done | `tools/slotflag.py` |
| Rebuilt kernel (namespaces, devtmpfs, SysV IPC) | Done | All 721 vendor modules load unchanged |
| SteamOS ARM 0.5.0 rootfs | Done | Debian 13 coexisted on the same partition |
| Valve system updates | No | Disabled on purpose, because they would overwrite the adaptation |
| Virtual terminals / SDDM | No | Kernel has no VT; replaced with own session starter |
| KVM | No | Qualcomm hypervisor owns EL2 |
| futex2 / ntsync for Wine | No | Kernel too old |
| nftables firewall | No | Not in kernel |

## Display and GPU

| Item | Status |
|---|---|
| Native resolution, 120 Hz, 10-bit, landscape | Done |
| Vulkan 1.4 (Turnip KGSL) | Done |
| OpenGL 4.6 (Zink) | Done |
| Native Freedreno GL scanout | Broken |
| Gamescope full session (Gaming Mode) | Done |
| Gamescope nested + per-game FSR | Done |
| Touch inside nested Gamescope | Broken |
| HDR, VRR | No |
| External display over USB-C (DP alt) | Todo, hard |

## Steam and games

| Item | Status |
|---|---|
| Steam Deck UI, sign-in, OOBE | Done |
| Proton 11 ARM64 + FEX, DXVK | Done (The Looker, Just Cause 3) |
| VKD3D (DX12) | Unverified |
| Native Linux x86 games | Todo, hard (needs an x86 Turnip-KGSL build) |
| MangoHud overlay | Broken |
| Quick Access: TDP, GPU clock, profile, charge limit | Done (set through the backend; sliders were reported to work) |
| Gaming ↔ Desktop switch | Done |
| Decky, Lutris, Heroic | Todo |
| Flatpak + Flathub | Done |

## Input and connectivity

| Item | Status |
|---|---|
| Touch | Done |
| Bluetooth + Xbox controller | Done |
| Controller as desktop mouse (Steam Input) | Done |
| Volume Up + Down opens Quick Access | Done |
| USB host mode | Done (Debian), Unverified (SteamOS) |
| Wi-Fi, hotspot | Done |
| Steam Deck controller emulation (InputPlumber) | Todo |
| On-screen keyboard in Gaming Mode | Todo |
| Mobile data, SMS, calls, GPS, NFC | No / hard |

## Media, power, sensors

| Item | Status |
|---|---|
| Haptics incl. OnePlus shaped effects | Done |
| HW video decode (driver level) | Done |
| HW HEVC gameplay recording (video only) | Done |
| HW video in browsers or players | Todo, hard |
| Internal speakers, earpiece, mics | Broken (no ALSA card) |
| Bluetooth / USB-C audio | Unverified |
| Thermal policy from OnePlus tables | Done |
| Whole-phone TDP limit | Done |
| Charge limit | Partial (couldn't be verified on a 500 mA port) |
| Fast charging | No (proprietary handshake) |
| Suspend | Broken |
| Sensors, cameras, fingerprint | No |
