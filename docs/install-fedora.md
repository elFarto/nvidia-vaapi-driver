# Installation on Fedora

Tested on Fedora 43 with NVIDIA driver 580.126.18 (RPM Fusion).

## Prerequisites

NVIDIA proprietary driver installed via RPM Fusion (`akmod-nvidia`).

Verify:
```bash
nvidia-smi --query-gpu=driver_version --format=csv,noheader
```

## Step 1 — Install build dependencies (64-bit)

```bash
sudo dnf install -y \
    meson ninja-build gcc pkg-config \
    libva-devel libdrm-devel mesa-libEGL-devel nv-codec-headers \
    libva-utils
```

## Step 2 — Install build dependencies (32-bit, for Steam)

```bash
sudo dnf install -y \
    glibc-devel.i686 \
    libva-devel.i686 libdrm-devel.i686 mesa-libEGL-devel.i686
```

## Step 3 — Remove stock libva-nvidia-driver

If you have the Fedora-packaged version (v0.0.16, decode-only), remove it first:

```bash
sudo dnf remove -y libva-nvidia-driver
```

## Step 4 — Build 64-bit

```bash
meson setup build64 . --wipe --prefix=/usr
meson compile -C build64
```

## Step 5 — Build 32-bit (cross-compile)

Fedora uses `/usr/lib/pkgconfig` for 32-bit `.pc` files (not `/usr/lib/i386-linux-gnu/`).
Create a cross-file:

```bash
cat > cross-i386-fedora.txt << 'EOF'
[binaries]
c = 'gcc'
cpp = 'g++'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-m32']
c_link_args = ['-m32']
cpp_args = ['-m32']
cpp_link_args = ['-m32']

[properties]
pkg_config_libdir = ['/usr/lib/pkgconfig', '/usr/share/pkgconfig']
sys_root = '/'

[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOF
```

Then build:

```bash
meson setup build32 . --wipe --cross-file cross-i386-fedora.txt
meson compile -C build32
```

## Step 6 — Install

```bash
sudo meson install -C build64
sudo mkdir -p /usr/lib/dri
sudo cp build32/nvidia_drv_video.so /usr/lib/dri/nvidia_drv_video.so
```

This installs:
- 64-bit driver → `/usr/lib64/dri/nvidia_drv_video.so`
- 32-bit driver → `/usr/lib/dri/nvidia_drv_video.so`
- nvenc-helper → `/usr/libexec/nvenc-helper`

## Step 7 — Systemd user service

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

## Step 8 — Verify

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
