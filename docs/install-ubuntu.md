# Installation on Ubuntu

Tested on Ubuntu 22.04+ with NVIDIA proprietary driver.

## Prerequisites

NVIDIA proprietary driver installed.

Verify:
```bash
nvidia-smi --query-gpu=driver_version --format=csv,noheader
```

Detect the driver version (used for 32-bit packages):
```bash
NV_VER=$(dpkg -l | grep 'libnvidia-compute-.*amd64' | awk '{print $2}' | sed 's/libnvidia-compute-//' | sed 's/:amd64//' | head -1)
echo "NVIDIA driver: $NV_VER"
```

## Step 1 — Install build dependencies (64-bit)

```bash
sudo apt-get install -y --no-install-recommends \
    meson ninja-build gcc pkg-config \
    libva-dev libdrm-dev libegl-dev libffmpeg-nvenc-dev \
    vainfo
```

## Step 2 — Install build dependencies (32-bit, for Steam)

```bash
sudo dpkg --add-architecture i386
sudo apt-get update

sudo apt-get install -y --no-install-recommends \
    gcc-multilib \
    libva-dev:i386 libdrm-dev:i386 libegl-dev:i386 \
    libnvidia-compute-${NV_VER}:i386 \
    libnvidia-encode-${NV_VER}:i386
```

## Step 3 — Build 64-bit

```bash
meson setup build64 . --wipe --prefix=/usr
meson compile -C build64
```

## Step 4 — Build 32-bit (cross-compile)

The repo includes `cross-i386.txt` configured for Ubuntu paths (`/usr/lib/i386-linux-gnu/`).

```bash
meson setup build32 . --wipe --cross-file cross-i386.txt
meson compile -C build32
```

## Step 5 — Install

```bash
sudo meson install -C build64
sudo mkdir -p /usr/lib/i386-linux-gnu/dri
sudo cp build32/nvidia_drv_video.so /usr/lib/i386-linux-gnu/dri/nvidia_drv_video.so
```

This installs:
- 64-bit driver → `/usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so`
- 32-bit driver → `/usr/lib/i386-linux-gnu/dri/nvidia_drv_video.so`
- nvenc-helper → `/usr/libexec/nvenc-helper`

## Step 6 — Systemd user service

```bash
mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/nvenc-helper.service << 'EOF'
[Unit]
Description=NVENC encode helper for nvidia-vaapi-driver
Documentation=https://github.com/efortin/nvidia-vaapi-driver
After=graphical-session.target

[Service]
Type=simple
ExecStart=/usr/libexec/nvenc-helper
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
EOF

systemctl --user daemon-reload
systemctl --user enable nvenc-helper.service
systemctl --user restart nvenc-helper.service
```

## Step 7 — Verify

```bash
# Check helper is running
systemctl --user is-active nvenc-helper.service

# Check VA-API profiles (should show VAEntrypointEncSlice for encode)
vainfo --display drm --device /dev/dri/renderD128
```

Expected output includes both decode (VLD) and encode (EncSlice) entrypoints:
```
VAProfileH264Main               :  VAEntrypointVLD
VAProfileH264Main               :  VAEntrypointEncSlice
VAProfileHEVCMain               :  VAEntrypointVLD
VAProfileHEVCMain               :  VAEntrypointEncSlice
```

No environment variables needed. Just launch Steam.
