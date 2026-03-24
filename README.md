# Sentinel-RT — Edge-Based Industrial Equipment Health Dashboard

**Sentinel-RT** is a real-time industrial equipment monitoring system running on a **Raspberry Pi 4** under **Linux with the PREEMPT_RT patch**. It continuously reads vibration, sound, temperature, and current sensors and serves the data over a **Mutual TLS (mTLS) encrypted TCP connection** to remote clients. Access is governed by a **Role-Based Access Control (RBAC)** system whose roles are embedded directly in X.509 client certificates.

> **Platform:** Raspberry Pi 4 Model B running Linux PREEMPT_RT (kernel 6.x-rt)
> **Language:** C (gcc, POSIX threads, OpenSSL)
> **Security:** mTLS 1.2/1.3 with OpenSSL, RBAC via X.509 OU field
> **Scheduling:** `SCHED_FIFO` RT worker threads + `mlockall()` for deterministic latency
> **Author:** Hemanth Kumar — [@Hemanthkumar04](https://github.com/Hemanthkumar04)

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Architecture Diagram](#2-architecture-diagram)
3. [Repository Structure](#3-repository-structure)
4. [Hardware Requirements and Wiring](#4-hardware-requirements-and-wiring)
5. [Host Machine Prerequisites](#5-host-machine-prerequisites)
6. [Linux-RT Kernel Setup](#6-linux-rt-kernel-setup)
7. [Enable I2C and 1-Wire in Boot Config](#7-enable-i2c-and-1-wire-in-boot-config)
8. [Clone the Repository and Generate Certificates](#8-clone-the-repository-and-generate-certificates)
9. [Build the Project](#9-build-the-project)
10. [Cross-Compile from x86-64](#10-cross-compile-from-x86-64)
11. [Deploy to the Raspberry Pi](#11-deploy-to-the-raspberry-pi)
12. [Run the Server](#12-run-the-server)
13. [Run the Client](#13-run-the-client)
14. [Command Reference](#14-command-reference)
15. [Security Architecture](#15-security-architecture)
16. [Sensor Specifications and Health Thresholds](#16-sensor-specifications-and-health-thresholds)
17. [Benchmarks Overview](#17-benchmarks-overview)
18. [Troubleshooting](#18-troubleshooting)
19. [Quick Reference Card](#19-quick-reference-card)

---

## 1. System Overview

### What the system does

Sentinel-RT turns a Raspberry Pi 4 into a secure, real-time edge monitoring node for industrial machinery. The Pi reads four sensor channels at different rates, evaluates equipment health, maintains a ring-buffer event log, and exposes all of this over an authenticated, encrypted TCP connection.

| Capability | Detail |
|---|---|
| Real-time sensor polling | 1 kHz digital GPIO for vibration and sound; 1 Hz I2C for current; 1 Hz 1-Wire for temperature |
| RT scheduling | `SCHED_FIFO` priority 80 sensor-polling thread; `mlockall(MCL_CURRENT | MCL_FUTURE)` eliminates page-fault latency |
| Transport security | Mutual TLS 1.2 / 1.3 over TCP port 8080; OpenSSL on both server and client |
| Access control | Four RBAC roles determined by the OU field of the client's X.509 certificate |
| Event logging | In-memory ring buffer of up to 512 CRITICAL events; retrieved with `get_log` |
| Live telemetry | Push-based `monitor [seconds]` command streams sensor readings at 1 s intervals |
| Multi-client | Thread-per-client architecture — each accepted TLS connection runs in its own POSIX thread |

### Why Linux PREEMPT_RT instead of a bare RTOS

Linux with the PREEMPT_RT patch provides microsecond-class worst-case interrupt and scheduling latency on the RPi 4 while retaining the full Linux userspace: standard POSIX APIs, OpenSSL, sysfs GPIO, i2c-dev, and the w1_therm kernel driver. This makes the codebase vastly simpler than a bare-metal Zephyr image that would need custom MbedTLS integration, hand-written device-tree overlays, and a separate Linux build for the client side. The measured worst-case latency on RPi 4 with a stock PREEMPT_RT kernel is typically under 100 µs, which is more than adequate for 1 kHz sensor polling with deterministic jitter.

---

## 2. Architecture Diagram

```
┌──────────────────────────────────────────────────────────┐
│           RASPBERRY PI 4  (Linux PREEMPT_RT)             │
│                                                          │
│  ┌──────────────┐   sysfs   ┌────────────────────────┐  │
│  │  SW-420      │──GPIO 17─►│                        │  │
│  │  Vibration   │           │   sensor_manager.c     │  │
│  ├──────────────┤   sysfs   │   (SCHED_FIFO prio 80) │  │
│  │  KY-038      │──GPIO 27─►│   1 kHz poll loop      │  │
│  │  Sound       │           │   health evaluation    │  │
│  ├──────────────┤  w1_therm │   ring-buffer log      │  │
│  │  DS18B20     │──GPIO  4─►│                        │  │
│  │  Temperature │           └──────────┬─────────────┘  │
│  ├──────────────┤   i2c-dev            │ SensorData      │
│  │  ACS712+     │──I2C bus1────────────┘                 │
│  │  ADS1115     │  (0x48)                                │
│  │  Current     │                                        │
│  └──────────────┘           ┌────────────────────────┐  │
│                              │   server.c             │  │
│                              │   OpenSSL mTLS :8080   │  │
│                              │   thread-per-client    │  │
│                              │   RBAC authorization   │  │
│                              └───────────┬────────────┘  │
└──────────────────────────────────────────┼───────────────┘
                                           │ mTLS / TCP 8080
           ┌───────────────────────────────┼──────────────┐
           │      CLIENT MACHINE           │              │
           │  (Linux / macOS / WSL)        ▼              │
           │                                              │
           │   $ ./ims_client <RPi_IP>                    │
           │   ┌──────────────────────────────────────┐   │
           │   │ ADMIN / OPERATOR / MAINTENANCE /     │   │
           │   │ VIEWER  (role from cert OU field)    │   │
           │   └──────────────────────────────────────┘   │
           └──────────────────────────────────────────────┘
```

### Data flow summary

1. The `sensor_manager` thread wakes every 1 ms (SCHED_FIFO, absolute `clock_nanosleep`).
2. GPIO sysfs reads vibration and sound; i2c-dev reads ADS1115 raw ADC; w1_therm reads DS18B20.
3. Readings are stored in a shared `SensorData` struct protected by a `pthread_mutex`.
4. `server.c` accepts a TCP connection, performs an OpenSSL mTLS handshake, extracts the client OU to determine the RBAC role, then dispatches the session to a new thread.
5. The session thread reads commands from the TLS stream and calls the appropriate handler in `protocol.c`.
6. The client `ims_client` presents its role-specific certificate, sends commands typed by the operator, and prints the server responses.

---

## 3. Repository Structure

```
Edge-Based-Industrial-Equipment-Health-Dashboard/
│
├── Makefile                    ← make server / make client / make certs / make all
│
├── apps/
│   ├── server.c                ← Linux-RT mTLS server (OpenSSL, SCHED_FIFO, thread-per-client)
│   └── client.c                ← Linux terminal client (OpenSSL mTLS, interactive prompt)
│
├── common/
│   ├── authorization.h         ← Role enum, role_name(), authorize_client() prototype
│   └── authorization.c         ← OpenSSL X.509 CN/OU extraction → RBAC role mapping
│
├── drivers/
│   ├── sensors.h               ← GPIO pin macros, SensorReading struct, HAL prototypes
│   ├── sensors.c               ← Linux sysfs GPIO + i2c-dev (ioctl) + w1_therm HAL
│   ├── sensor_manager.h        ← SensorManager struct, start/stop API
│   └── sensor_manager.c        ← SCHED_FIFO polling thread, health scoring, event log
│
├── protocol/
│   ├── protocol.h              ← ProtocolContext (SSL* + role), command handler prototypes
│   └── protocol.c              ← handle_command(), monitor loop, log ring buffer helpers
│
├── scripts/
│   └── quick_start.sh          ← Cert generation (openssl CLI) + build + optional deploy
│
├── certs/                      ← Generated by quick_start.sh (gitignored)
│   ├── ca.crt / ca.key
│   ├── server.crt / server.key
│   ├── admin_client.crt / admin_client.key
│   ├── operator_client.crt / operator_client.key
│   ├── maintenance_client.crt / maintenance_client.key
│   ├── viewer_client.crt / viewer_client.key
│   └── client.crt / client.key ← Symlink → admin_client (default for ims_client)
│
└── tests/
    ├── bench_sha_qnx.c               ← SHA-256 benchmark for QNX Neutrino
    ├── bench_sha_linuxrt.c           ← SHA-256 benchmark for Linux-RT
    ├── bench_matrix1_qnx.c           ← Matrix multiply benchmark for QNX
    ├── bench_matrix1_linuxrt.c       ← Matrix multiply benchmark for Linux-RT
    ├── bench_md5_qnx.c               ← MD5 benchmark for QNX
    ├── bench_md5_linuxrt.c           ← MD5 benchmark for Linux-RT
    ├── bench_binarysearch_qnx.c      ← Binary search benchmark for QNX
    ├── bench_binarysearch_linuxrt.c  ← Binary search benchmark for Linux-RT
    ├── bench_fir2dim_qnx.c           ← 2-D FIR filter benchmark for QNX
    └── bench_fir2dim_linuxrt.c       ← 2-D FIR filter benchmark for Linux-RT
```

---

## 4. Hardware Requirements and Wiring

### Bill of Materials

| # | Component | Model / Part | Purpose |
|---|---|---|---|
| 1 | Single-board computer | Raspberry Pi 4 Model B (1/2/4/8 GB) | Runs Linux-RT server |
| 2 | Vibration sensor | SW-420 normally-closed module | Detects mechanical shock |
| 3 | Sound sensor | KY-038 microphone comparator module | Acoustic duty-cycle monitoring |
| 4 | Temperature sensor | DS18B20 (TO-92 or waterproof probe) | 1-Wire digital temperature |
| 5 | Current sensor | ACS712-20A Hall-effect module | Measures motor/load current |
| 6 | ADC | ADS1115 16-bit I2C ADC breakout (0x48) | Digitises ACS712 analogue output |
| 7 | Pull-up resistor | 4.7 kΩ through-hole resistor | DS18B20 1-Wire line pull-up |
| 8 | Micro-SD card | 8 GB+, Class 10 or better | Operating system + application |
| 9 | Power supply | Official RPi 4 USB-C 5V/3A | Stable power under I2C load |
| 10 | Jumper wires | Male-to-female and male-to-male | Breadboard connections |

### RPi 4 GPIO Header Reference (selected pins)

```
         3V3  (1) (2)  5V
 SDA1/GPIO 2  (3) (4)  5V
 SCL1/GPIO 3  (5) (6)  GND
 1Wire/GPIO 4 (7) (8)  GPIO14 (UART TX)
          GND (9) (10) GPIO15 (UART RX)
  Vib/GPIO17 (11) (12) GPIO18
  Snd/GPIO27 (13) (14) GND
         3V3 (17) (18) GPIO24
         GND (25) (26) GPIO7
```

### Wiring Tables

#### SW-420 Vibration Sensor → RPi 4

| SW-420 Pin | RPi 4 Physical Pin | BCM GPIO | Note |
|---|---|---|---|
| VCC | Pin 1 | 3.3 V | Module rated 3.3–5 V |
| GND | Pin 6 | GND | |
| DO | Pin 11 | GPIO 17 | Digital output, active LOW on vibration |

#### KY-038 Sound Sensor → RPi 4

| KY-038 Pin | RPi 4 Physical Pin | BCM GPIO | Note |
|---|---|---|---|
| VCC | Pin 1 | 3.3 V | |
| GND | Pin 6 | GND | |
| DO | Pin 13 | GPIO 27 | Adjust the blue trimmer pot so DO goes HIGH only on loud noise |

The KY-038 has both a digital output (DO, comparator-based) and an analogue output (AO). Sentinel-RT uses only the digital output (DO).

#### DS18B20 Temperature Sensor → RPi 4

| DS18B20 Pin | RPi 4 Physical Pin | BCM GPIO | Note |
|---|---|---|---|
| VDD | Pin 1 | 3.3 V | Use normal power mode (not parasite) |
| GND | Pin 9 | GND | |
| DATA | Pin 7 | GPIO 4 | **4.7 kΩ pull-up mandatory between VDD and DATA** |

```
  3.3V ──┬──[4.7 kΩ]──┬── GPIO 4
         │             │
        VDD           DATA    (DS18B20)
        GND ───────── GND
```

#### ACS712 Current Sensor → ADS1115 ADC → RPi 4

| Connection | Detail |
|---|---|
| ACS712 VCC | 5 V (RPi Pin 2) |
| ACS712 GND | GND (RPi Pin 6) |
| ACS712 OUT | ADS1115 A0 (analogue input) |
| ADS1115 VDD | 3.3 V (RPi Pin 1) |
| ADS1115 GND | GND (RPi Pin 6) |
| ADS1115 SDA | GPIO 2 / SDA1 (RPi Pin 3) |
| ADS1115 SCL | GPIO 3 / SCL1 (RPi Pin 5) |
| ADS1115 ADDR | GND → I2C address 0x48 |

The ACS712-20A outputs 2.5 V at 0 A with a sensitivity of 100 mV/A. The ADS1115 is configured for ±4.096 V full-scale range in single-ended mode on AIN0. The driver converts the raw 16-bit ADC value to amps using: `current_A = (adc_raw * 4.096 / 32768.0 - 2.5) / 0.1`.

---

## 5. Host Machine Prerequisites

All prerequisites must be installed on the **Raspberry Pi 4** itself (the build and runtime machine). If you are cross-compiling from an x86-64 host, see Section 10.

### Install required packages on the RPi (Raspberry Pi OS Bookworm / Debian 12)

```bash
sudo apt update && sudo apt upgrade -y

# Compiler and build tools
sudo apt install -y gcc make

# OpenSSL development library (required for both server and client builds)
sudo apt install -y libssl-dev openssl

# I2C userspace tools (for i2cdetect, i2cget — useful for debugging)
sudo apt install -y i2c-tools

# 1-Wire tools (optional but useful for diagnosing DS18B20)
sudo apt install -y python3-w1thermsensor

# Git (to clone the repository)
sudo apt install -y git
```

### Verify OpenSSL

```bash
openssl version
# Expected: OpenSSL 3.x.x  (Bookworm ships 3.0)
```

### Verify I2C tools

```bash
sudo i2cdetect -y 1
# You should see 0x48 in the grid if the ADS1115 is wired correctly
```

---

## 6. Linux-RT Kernel Setup

The PREEMPT_RT patch transforms the Linux kernel's interrupt handlers and spinlocks into preemptible threads, reducing worst-case scheduling latency from tens of milliseconds (vanilla Linux) to tens of microseconds. This is essential for the 1 kHz sensor polling thread in Sentinel-RT to meet its timing budget.

### Option A — Install the pre-built RT kernel (recommended for most users)

Raspberry Pi OS Bookworm provides a pre-built PREEMPT_RT kernel in its repositories. This is the easiest path:

```bash
sudo apt update

# Install the RT kernel image and headers
sudo apt install -y linux-image-rt-arm64 linux-headers-rt-arm64

# Reboot into the RT kernel
sudo reboot
```

After reboot, verify:

```bash
uname -a
# Should contain "PREEMPT_RT" in the output, e.g.:
# Linux raspberrypi 6.6.x-rt-v8+ #1 SMP PREEMPT_RT ...

cat /sys/kernel/realtime
# Should print: 1
```

If `linux-image-rt-arm64` is not found, try the Raspberry Pi Foundation's own RT kernel:

```bash
sudo apt install -y raspberrypi-kernel-rt
sudo reboot
```

### Option B — Build PREEMPT_RT from source

Use this path if you need a custom kernel configuration or a specific RT patch version.

**Step 1 — Install build dependencies (on the RPi or a cross-compile host):**

```bash
sudo apt install -y bc bison flex libssl-dev make gcc \
    libncurses-dev git wget xz-utils
```

**Step 2 — Download the kernel source and matching RT patch:**

```bash
# Find a matching RT patch at https://cdn.kernel.org/pub/linux/kernel/projects/rt/
# Example for kernel 6.6.x:
KERNEL_VERSION=6.6.31
RT_PATCH=patch-6.6.31-rt31.patch.xz

wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz
wget https://cdn.kernel.org/pub/linux/kernel/projects/rt/6.6/${RT_PATCH}

tar xf linux-${KERNEL_VERSION}.tar.xz
cd linux-${KERNEL_VERSION}
xzcat ../${RT_PATCH} | patch -p1
```

**Step 3 — Configure with PREEMPT_RT enabled:**

```bash
# Start from the RPi default config
make bcm2711_defconfig

# Open menuconfig and set: General setup → Preemption Model → Fully Preemptible Kernel (Real-Time)
make menuconfig

# Or set it non-interactively:
scripts/config --enable PREEMPT_RT
scripts/config --disable PREEMPT_VOLUNTARY
scripts/config --disable PREEMPT
```

**Step 4 — Build and install:**

```bash
# On the RPi (slow — allow 2+ hours):
make -j4 Image.gz modules dtbs
sudo make modules_install
sudo cp arch/arm64/boot/Image.gz /boot/kernel8.img
sudo cp arch/arm64/boot/dts/broadcom/bcm2711-rpi-4-b.dtb /boot/
sudo reboot
```

**Step 5 — Verify (same as Option A):**

```bash
uname -a          # Must contain PREEMPT_RT
cat /sys/kernel/realtime   # Must print 1
```

### Set RT scheduling permissions for non-root users (optional)

By default, only root can set `SCHED_FIFO`. The server runs as root, so this is not required. If you want to test benchmarks as a normal user:

```bash
# Add to /etc/security/limits.conf:
sudo tee -a /etc/security/limits.conf <<'EOF'
*  hard  rtprio  99
*  soft  rtprio  99
EOF
```

---

## 7. Enable I2C and 1-Wire in Boot Config

Edit `/boot/config.txt` (or `/boot/firmware/config.txt` on newer Raspberry Pi OS images):

```bash
sudo nano /boot/config.txt
```

Add or uncomment these lines:

```ini
# Enable I2C bus 1 (GPIO 2 = SDA, GPIO 3 = SCL)
dtparam=i2c_arm=on
dtparam=i2c_arm_baudrate=400000

# Enable 1-Wire on GPIO 4 (DS18B20)
dtoverlay=w1-gpio,gpiopin=4

# Optional: ensure SPI is off to reduce IRQ noise during benchmarks
dtparam=spi=off
```

Save the file and reboot:

```bash
sudo reboot
```

After reboot, verify:

```bash
# I2C — should show /dev/i2c-1
ls /dev/i2c-*

# 1-Wire — should show a directory named 28-xxxxxxxxxxxx
ls /sys/bus/w1/devices/

# Read the temperature directly
cat /sys/bus/w1/devices/28-*/w1_slave
# Output looks like:
# 50 05 55 05 7f ff 0c 10 1c : crc=1c YES
# 50 05 55 05 7f ff 0c 10 1c t=21312
# The t= value is temperature in millidegrees Celsius (21.312 °C)
```

---

## 8. Clone the Repository and Generate Certificates

```bash
# Clone the repository
git clone https://github.com/Hemanthkumar04/Edge-Based-Industrial-Equipment-Health-Dashboard.git
cd Edge-Based-Industrial-Equipment-Health-Dashboard

# Make the helper script executable
chmod +x scripts/quick_start.sh

# Generate all TLS certificates (CA, server, and four role-based client certs)
./scripts/quick_start.sh
```

The `quick_start.sh` script performs the following steps using the `openssl` CLI:

1. Creates a self-signed Certificate Authority (`certs/ca.crt` + `certs/ca.key`)
2. Generates the server certificate signed by the CA (`certs/server.crt` + `certs/server.key`)
3. Generates four client certificates, one per RBAC role, each with a distinct OU field:
   - `OU=ADMIN` → `certs/admin_client.crt / admin_client.key`
   - `OU=OPERATOR` → `certs/operator_client.crt / operator_client.key`
   - `OU=MAINTENANCE` → `certs/maintenance_client.crt / maintenance_client.key`
   - `OU=VIEWER` → `certs/viewer_client.crt / viewer_client.key`
4. Creates a symlink `certs/client.crt` → `certs/admin_client.crt` (default identity for `ims_client`)

> **Security note:** The `certs/` directory is listed in `.gitignore`. Never commit private keys to version control.

---

## 9. Build the Project

### Build both server and client

```bash
make
# Equivalent to: make server && make client
```

### Build only the server

```bash
make server
# Produces: ./ims_server
# Compiler flags: gcc -O2 -Wall -Wextra -lpthread -lssl -lcrypto
```

### Build only the client

```bash
make client
# Produces: ./ims_client
# Compiler flags: gcc -O2 -Wall -Wextra -lssl -lcrypto
```

### Generate certificates via Make

```bash
make certs
# Runs scripts/quick_start.sh internally
```

### Clean build artifacts

```bash
make clean
```

### Expected output after `make`

```
gcc -O2 -Wall -Wextra -Icommon -Idrivers -Iprotocol \
    apps/server.c common/authorization.c drivers/sensors.c \
    drivers/sensor_manager.c protocol/protocol.c \
    -o ims_server -lpthread -lssl -lcrypto
gcc -O2 -Wall -Wextra -Icommon \
    apps/client.c common/authorization.c \
    -o ims_client -lssl -lcrypto
```

---

## 10. Cross-Compile from x86-64

If you prefer to build on a faster x86-64 workstation and copy the binaries to the RPi:

### Install the cross-compiler

```bash
# Ubuntu/Debian x86-64 host
sudo apt install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu

# Install OpenSSL for AArch64 (sysroot approach)
# Easiest: use a Docker container with arm64 Debian, or install multiarch libs
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install -y libssl-dev:arm64
```

### Cross-compile

```bash
CC=aarch64-linux-gnu-gcc make
# or, if your OpenSSL arm64 headers are in a custom sysroot:
CC=aarch64-linux-gnu-gcc \
  CFLAGS="--sysroot=/usr/aarch64-linux-gnu" \
  LDFLAGS="--sysroot=/usr/aarch64-linux-gnu" \
  make
```

### Copy to the RPi

```bash
scp ims_server ims_client pi@<RPi_IP>:~/sentinel-rt/
```

---

## 11. Deploy to the Raspberry Pi

The `quick_start.sh` script has an optional `deploy` subcommand that copies all necessary files (binaries, certificates, and a systemd unit) to the RPi over SSH:

```bash
./scripts/quick_start.sh deploy <RPi_IP>
# Example:
./scripts/quick_start.sh deploy 192.168.1.42
```

This command performs:
1. `scp ims_server ims_client pi@<IP>:~/sentinel-rt/`
2. `scp -r certs/ pi@<IP>:~/sentinel-rt/certs/`
3. Optionally installs a systemd service so `ims_server` starts on boot

### Manual deployment

```bash
RPi_IP=192.168.1.42

# Create the target directory
ssh pi@${RPi_IP} "mkdir -p ~/sentinel-rt/certs"

# Copy binaries
scp ims_server ims_client pi@${RPi_IP}:~/sentinel-rt/

# Copy certificates
scp certs/ca.crt certs/server.crt certs/server.key pi@${RPi_IP}:~/sentinel-rt/certs/

# Set correct permissions on the private key
ssh pi@${RPi_IP} "chmod 600 ~/sentinel-rt/certs/server.key"
```

---

## 12. Run the Server

The server requires root privileges for two reasons:
1. `SCHED_FIFO` scheduling (requires `CAP_SYS_NICE`)
2. GPIO sysfs export (requires write access to `/sys/class/gpio/export`)

```bash
# On the Raspberry Pi
cd ~/sentinel-rt
sudo ./ims_server
```

### Expected startup output

```
[IMS] Sentinel-RT server starting...
[IMS] mlockall() OK — memory locked for RT operation
[IMS] Sensor manager initialised (GPIO17=vibration, GPIO27=sound, GPIO4=1-Wire, I2C1=current)
[IMS] SCHED_FIFO priority 80 set for sensor polling thread
[IMS] OpenSSL mTLS context loaded — server.crt / ca.crt
[IMS] Listening on 0.0.0.0:8080
[IMS] Ready — waiting for client connections
```

### Run as a systemd service (autostart on boot)

Create `/etc/systemd/system/sentinel-rt.service`:

```ini
[Unit]
Description=Sentinel-RT Industrial Equipment Health Dashboard
After=network.target

[Service]
Type=simple
ExecStart=/home/pi/sentinel-rt/ims_server
WorkingDirectory=/home/pi/sentinel-rt
User=root
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable sentinel-rt
sudo systemctl start sentinel-rt
sudo systemctl status sentinel-rt
```

---

## 13. Run the Client

The client does not need root privileges. It accepts the server IP address as a mandatory argument.

```bash
# Default: uses certs/client.crt (symlink to admin_client.crt)
./ims_client <RPi_IP>

# Example
./ims_client 192.168.1.42

# Use a specific role certificate
CERT=certs/operator_client.crt KEY=certs/operator_client.key ./ims_client 192.168.1.42
```

### Expected connection output

```
[CLIENT] Connecting to 192.168.1.42:8080...
[CLIENT] TLS handshake complete — TLS 1.3, AES_256_GCM_SHA384
[CLIENT] Authenticated as: admin_user (Role: ADMIN)

Sentinel-RT> help
```

---

## 14. Command Reference

All commands are typed at the `Sentinel-RT>` prompt in the client terminal. Commands that require elevated roles return `ERROR: permission denied` if the authenticated role does not have access.

| Command | Minimum Role | Description |
|---|---|---|
| `help` | VIEWER | Print a summary of available commands |
| `whoami` | VIEWER | Display the authenticated username and RBAC role |
| `list_units` | VIEWER | List all monitored equipment units and their current health status |
| `get_sensors` | VIEWER | Return a single snapshot of all four sensor readings |
| `get_health` | VIEWER | Return the computed health score and status string for each unit |
| `get_log` | OPERATOR | Retrieve the last N entries from the event ring buffer |
| `monitor [time]` | OPERATOR | Stream live sensor data for `time` seconds (default: 30 s); Ctrl-C to stop early |
| `clear_log` | ADMIN | Flush the event ring buffer |
| `quit` | VIEWER | Close the TLS session gracefully |

### Example session

```
Sentinel-RT> whoami
Username : admin_user
Role     : ADMIN
CN       : admin_user
OU       : ADMIN

Sentinel-RT> get_sensors
Timestamp  : 2026-03-24T14:22:07Z
Vibration  : 0  (NORMAL — no shock detected)
Sound      : 1  (ALERT — noise threshold exceeded)
Temperature: 42.312 °C
Current    : 3.74 A

Sentinel-RT> get_health
Unit 0 — Motor A
  Vibration  : OK
  Sound      : WARNING (threshold: 0, reading: 1)
  Temperature: OK  (threshold: 75 °C, reading: 42.3 °C)
  Current    : OK  (threshold: 15 A, reading: 3.7 A)
  Overall    : WARNING

Sentinel-RT> monitor 5
[14:22:10] V=0 S=1 T=42.4°C I=3.75A  HEALTH=WARNING
[14:22:11] V=0 S=0 T=42.4°C I=3.76A  HEALTH=OK
[14:22:12] V=0 S=0 T=42.5°C I=3.74A  HEALTH=OK
[14:22:13] V=0 S=0 T=42.5°C I=3.75A  HEALTH=OK
[14:22:14] V=1 S=0 T=42.5°C I=3.75A  HEALTH=WARNING
Monitor session ended (5 s).
```

---

## 15. Security Architecture

### Mutual TLS

Both server and client authenticate with X.509 certificates signed by the same private CA. Neither party will proceed past the TLS handshake without a valid certificate from the shared CA. This prevents rogue clients and rogue servers.

- CA key pair: `certs/ca.crt` / `certs/ca.key`
- Server certificate: signed by the CA, presented to clients for server authentication
- Client certificates: four variants (one per role), each signed by the CA, presented to the server for client authentication

The TLS context on the server is configured with:
- `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL)` — mandatory mutual authentication
- `SSL_CTX_load_verify_locations(ctx, "certs/ca.crt", NULL)` — only certs signed by this CA are accepted

### RBAC Role Mapping

After a successful TLS handshake, the server calls `authorize_client()` in `common/authorization.c`. This function uses the OpenSSL API to extract the `OU` (Organisational Unit) field from the verified client certificate's Subject Distinguished Name. The OU value is mapped to one of four roles:

| Certificate OU | RBAC Role | Numeric Level |
|---|---|---|
| `ADMIN` | Administrator | 4 (highest) |
| `OPERATOR` | Operator | 3 |
| `MAINTENANCE` | Maintenance technician | 2 |
| `VIEWER` | Read-only observer | 1 (lowest) |

The `authorize_client()` function returns the numeric level, which `protocol.c` checks against a per-command minimum level before dispatching the handler.

### Certificate Generation Details

The `quick_start.sh` script uses `openssl req` and `openssl x509` to create all certificates. Key parameters:

- Key algorithm: RSA 2048-bit (or EC P-256 if preferred)
- Validity: 3650 days (10 years) for CA; 365 days for leaf certificates
- Subject for admin client: `/CN=admin_user/OU=ADMIN/O=SentinelRT/C=IN`
- Subject for viewer client: `/CN=viewer_user/OU=VIEWER/O=SentinelRT/C=IN`

### Network exposure

The server binds to `0.0.0.0:8080` by default. In a production deployment, restrict access using `iptables`:

```bash
# Allow only connections from the trusted management network
sudo iptables -A INPUT -p tcp --dport 8080 -s 192.168.1.0/24 -j ACCEPT
sudo iptables -A INPUT -p tcp --dport 8080 -j DROP
```

---

## 16. Sensor Specifications and Health Thresholds

### SW-420 Vibration Sensor

| Parameter | Value |
|---|---|
| Operating voltage | 3.3 V – 5 V |
| Output type | Digital (DO), active HIGH when vibration detected |
| Interface | GPIO 17 (sysfs `/sys/class/gpio/gpio17/value`) |
| Poll rate | 1 kHz (read every 1 ms in the sensor manager thread) |
| Health threshold | Any `1` reading → VIBRATION_ALERT event logged |
| Sensitivity | Adjustable via onboard trimmer potentiometer |

### KY-038 Sound Sensor

| Parameter | Value |
|---|---|
| Operating voltage | 3.3 V – 5 V |
| Output type | Digital (DO), HIGH above acoustic threshold |
| Interface | GPIO 27 (sysfs `/sys/class/gpio/gpio27/value`) |
| Poll rate | 1 kHz |
| Health threshold | Sustained HIGH for > 100 ms → SOUND_ALERT event |
| Sensitivity | Set trimmer until DO is LOW in ambient noise, HIGH at loud events |

### DS18B20 Temperature Sensor

| Parameter | Value |
|---|---|
| Operating voltage | 3.3 V (normal power mode) |
| Interface | 1-Wire on GPIO 4, via kernel `w1_therm` driver |
| Kernel sysfs path | `/sys/bus/w1/devices/28-*/w1_slave` |
| Poll rate | 1 Hz (conversion takes ~750 ms at 12-bit resolution) |
| Resolution | 12-bit, 0.0625 °C per LSB |
| Operating range | −55 °C to +125 °C |
| Health thresholds | WARNING: > 70 °C; CRITICAL: > 85 °C |

### ACS712 + ADS1115 Current Measurement

| Parameter | Value |
|---|---|
| ACS712 variant | 20 A bidirectional Hall-effect sensor |
| ACS712 sensitivity | 100 mV/A |
| ACS712 zero-current output | 2.5 V (nominal) |
| ADS1115 I2C address | 0x48 |
| ADS1115 PGA setting | ±4.096 V full-scale |
| ADS1115 resolution | 16-bit → 0.125 mV/LSB at ±4.096 V |
| I2C bus | `/dev/i2c-1` via `ioctl(fd, I2C_RDWR, ...)` |
| Poll rate | 1 Hz |
| Health thresholds | WARNING: > 12 A; CRITICAL: > 18 A |

### Conversion formula

```c
// Raw 16-bit signed integer from ADS1115 AIN0 in single-ended mode
// PGA = ±4.096 V → LSB = 4.096 / 32768 V
double voltage = adc_raw * (4.096 / 32768.0);
double current_A = (voltage - 2.5) / 0.100;   // 100 mV/A sensitivity
```

---

## 17. Benchmarks Overview

The `tests/` directory contains pairs of RT-bench programs ported to both **QNX Neutrino 8.0** and **Linux-RT**. These are used to quantitatively compare the two platforms' real-time characteristics. See `benchmark_test.md` in this repository for the full methodology, build instructions, results tables, and a Python plotting script.

### Benchmark programs

| Test | QNX source | Linux-RT source | What it measures |
|---|---|---|---|
| SHA-256 | `bench_sha_qnx.c` | `bench_sha_linuxrt.c` | Cryptographic hash execution time jitter |
| MATRIX1 | `bench_matrix1_qnx.c` | `bench_matrix1_linuxrt.c` | Dense matrix multiply execution time jitter |
| MD5 | `bench_md5_qnx.c` | `bench_md5_linuxrt.c` | MD5 hash execution time jitter |
| BINARYSEARCH | `bench_binarysearch_qnx.c` | `bench_binarysearch_linuxrt.c` | Sorted-array binary search execution time jitter |
| FIR2DIM | `bench_fir2dim_qnx.c` | `bench_fir2dim_linuxrt.c` | 2-D FIR filter execution time jitter |

### Build and run the Linux-RT benchmarks

```bash
cd tests/

# Build a single benchmark
gcc -O2 -o bench_sha bench_sha_linuxrt.c -lm

# Run with SCHED_FIFO (requires root or rtprio capability)
sudo chrt -f 80 ./bench_sha

# Build all Linux-RT benchmarks at once
for b in sha matrix1 md5 binarysearch fir2dim; do
    gcc -O2 -o bench_${b} bench_${b}_linuxrt.c -lm
done
```

---

## 18. Troubleshooting

### 1. `sudo ./ims_server` exits immediately with "mlockall failed: ENOMEM"

The process ran out of lockable memory. Increase the memlock limit:

```bash
sudo tee -a /etc/security/limits.conf <<'EOF'
root  hard  memlock  unlimited
root  soft  memlock  unlimited
EOF
# Log out and back in, then retry
sudo ./ims_server
```

### 2. `./ims_client` fails with "SSL handshake error: certificate verify failed"

The client certificate was not signed by the CA the server trusts, or the certificate has expired. Regenerate all certificates:

```bash
rm -rf certs/
./scripts/quick_start.sh
```

Then re-deploy `certs/ca.crt`, `certs/server.crt`, and `certs/server.key` to the RPi.

### 3. `cat /sys/kernel/realtime` prints nothing or the file does not exist

The running kernel does not have PREEMPT_RT. Check `uname -a`; if it does not contain "PREEMPT_RT", reinstall the RT kernel as described in Section 6 and reboot.

### 4. I2C address 0x48 not visible in `i2cdetect -y 1`

- Confirm `/boot/config.txt` contains `dtparam=i2c_arm=on` and the system has been rebooted.
- Check wiring: SDA to Pin 3, SCL to Pin 5, VDD to 3.3 V.
- Verify the ADS1115 ADDR pin is tied to GND (for address 0x48). Tying ADDR to VDD gives 0x49.

### 5. DS18B20 not appearing in `/sys/bus/w1/devices/`

- Confirm `/boot/config.txt` contains `dtoverlay=w1-gpio,gpiopin=4`.
- Confirm the 4.7 kΩ pull-up resistor is present between the DATA line and 3.3 V.
- After editing `/boot/config.txt`, a reboot is required before the overlay takes effect.
- Check that the `w1_therm` and `wire` kernel modules are loaded: `lsmod | grep w1`.

### 6. `drivers/sensors.c` compile error: "i2c/smbus.h: No such file or directory"

Install the i2c-tools development package:

```bash
sudo apt install -y libi2c-dev
```

### 7. Server reports "SCHED_FIFO: Operation not permitted"

The server process does not have the `CAP_SYS_NICE` capability. Run it as root (`sudo ./ims_server`) or grant the capability to the binary:

```bash
sudo setcap cap_sys_nice+ep ./ims_server
./ims_server   # No sudo needed after this
```

### 8. `get_sensors` returns temperature = -999 or "sensor error"

The w1_therm file exists but the CRC check fails (the line ends in `NO`). Common causes:
- Missing or wrong-value pull-up resistor (must be 4.7 kΩ, not 10 kΩ).
- Long wire length causing signal integrity issues — shorten or use a proper twisted-pair cable.
- Multiple DS18B20 sensors on the same bus with address collision — check each sensor's 64-bit ROM code.

### 9. Client hangs after "Connecting to..."

- Verify the server is running: `ssh pi@<IP> "pgrep ims_server"`.
- Check firewall: `sudo iptables -L -n | grep 8080`. If the port is blocked, allow it: `sudo iptables -A INPUT -p tcp --dport 8080 -j ACCEPT`.
- Confirm the RPi IP address is correct: `ssh pi@<IP> "hostname -I"`.

### 10. Vibration sensor always reads 0 (or always reads 1)

- If always 0: the sensor's sensitivity trimmer is set too high. Tap the module and watch if it ever triggers.
- If always 1: the trimmer is set too low, triggering constantly. Turn the trimmer clockwise to increase the threshold.
- Verify the DO pin is connected to GPIO 17 (physical pin 11, not physical pin 12 which is GPIO 18).

### 11. `make` fails with "libssl not found"

```bash
sudo apt install -y libssl-dev
# If on a 64-bit OS building for 64-bit:
ldconfig -p | grep libssl
```

### 12. `monitor` command disconnects after exactly 30 seconds even when a longer time was requested

Check the `DEFAULT_MONITOR_TIMEOUT` constant in `protocol/protocol.h`. The monitor loop enforces a server-side maximum. ADMIN and OPERATOR roles may have a longer limit than VIEWER.

---

## 19. Quick Reference Card

```
┌──────────────────────────────────────────────────────────────────┐
│                   SENTINEL-RT QUICK REFERENCE                    │
├──────────────────────────────────────────────────────────────────┤
│  PREREQUISITES (RPi 4)                                           │
│  sudo apt install gcc libssl-dev openssl i2c-tools               │
│                                                                  │
│  RT KERNEL CHECK                                                 │
│  uname -a              → must contain PREEMPT_RT                 │
│  cat /sys/kernel/realtime  → must print 1                        │
│                                                                  │
│  /boot/config.txt                                                │
│  dtparam=i2c_arm=on                                              │
│  dtoverlay=w1-gpio,gpiopin=4                                     │
│                                                                  │
│  BUILD                                                           │
│  ./scripts/quick_start.sh   ← generate certs                    │
│  make                        ← build server + client             │
│  CC=aarch64-linux-gnu-gcc make  ← cross-compile                 │
│                                                                  │
│  DEPLOY                                                          │
│  ./scripts/quick_start.sh deploy <IP>                            │
│                                                                  │
│  RUN                                                             │
│  sudo ./ims_server            ← on RPi (root required)           │
│  ./ims_client <RPi_IP>        ← on client machine                │
│                                                                  │
│  COMMANDS                                                        │
│  help / whoami / list_units / get_sensors / get_health           │
│  get_log  (OPERATOR+)                                            │
│  monitor [sec]  (OPERATOR+)                                      │
│  clear_log  (ADMIN only)                                         │
│  quit                                                            │
│                                                                  │
│  GPIO PINS                                                       │
│  GPIO 17  ← SW-420 vibration (DO)                               │
│  GPIO 27  ← KY-038 sound (DO)                                   │
│  GPIO  4  ← DS18B20 temperature (1-Wire)                        │
│  GPIO  2  ← I2C SDA (ADS1115)                                   │
│  GPIO  3  ← I2C SCL (ADS1115)                                   │
│                                                                  │
│  RBAC ROLES (cert OU field)                                      │
│  ADMIN > OPERATOR > MAINTENANCE > VIEWER                         │
│                                                                  │
│  HEALTH THRESHOLDS                                               │
│  Temperature: WARNING >70°C  CRITICAL >85°C                     │
│  Current    : WARNING >12 A  CRITICAL >18 A                     │
│  Vibration  : any pulse → ALERT                                  │
│  Sound      : sustained >100 ms → ALERT                          │
└──────────────────────────────────────────────────────────────────┘
```
