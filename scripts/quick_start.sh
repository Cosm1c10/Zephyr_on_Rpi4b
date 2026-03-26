#!/bin/bash
# ============================================================================
# IMS Quick Start Script  —  Linux-RT (PREEMPT_RT) / Raspberry Pi 4
# ============================================================================
#
# Usage:
#   ./scripts/quick_start.sh              # generate certs + build server & client
#   ./scripts/quick_start.sh certs        # cert generation only
#   ./scripts/quick_start.sh build        # build only (certs must exist)
#   ./scripts/quick_start.sh deploy <IP>  # scp server binary to RPi 4
#   ./scripts/quick_start.sh rt-check     # verify PREEMPT_RT on connected RPi
#
# Prerequisites (RPi 4 or build host):
#   sudo apt install gcc libssl-dev openssl
#
# Prerequisites (cross-compile from x86-64):
#   sudo apt install gcc-aarch64-linux-gnu libssl-dev
#   CC=aarch64-linux-gnu-gcc make
# ============================================================================

set -e
cd "$(dirname "$0")/.."

OPENSSL_CMD="${OPENSSL_CMD:-openssl}"
export OPENSSL_CONF="${OPENSSL_CONF:-/etc/ssl/openssl.cnf}"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()   { echo -e "${GREEN}[INFO]  $1${NC}"; }
warn()  { echo -e "${YELLOW}[WARN]  $1${NC}"; }
error() { echo -e "${RED}[ERROR] $1${NC}"; exit 1; }

# ============================================================================
# 1. Generate TLS Certificates
# ============================================================================
generate_certs() {
    log "Generating TLS certificates in certs/ ..."
    mkdir -p certs

    command -v openssl &>/dev/null || error "openssl not found. Install with: sudo apt install openssl"

    # CA
    if [ ! -f "certs/ca.key" ]; then
        $OPENSSL_CMD genrsa -out certs/ca.key 2048 2>/dev/null
        $OPENSSL_CMD req -x509 -new -nodes -key certs/ca.key \
            -sha256 -days 365 -out certs/ca.crt \
            -subj "/CN=IMS_Root_CA"
        log "CA certificate created."
    else
        log "CA certificate already exists, skipping."
    fi

    # Server certificate
    if [ ! -f "certs/server.key" ]; then
        $OPENSSL_CMD genrsa -out certs/server.key 2048 2>/dev/null
        $OPENSSL_CMD req -new -key certs/server.key \
            -out certs/server.csr \
            -subj "/CN=ims_server/O=IMS"
        $OPENSSL_CMD x509 -req -in certs/server.csr \
            -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial \
            -out certs/server.crt -days 365 -sha256
        log "Server certificate created."
    else
        log "Server certificate already exists, skipping."
    fi

    # Client certificates (one per role)
    ROLES=("admin:ADMIN" "operator:OPERATOR" "viewer:VIEWER" "maintenance:MAINTENANCE")
    for role_info in "${ROLES[@]}"; do
        role_name="${role_info%%:*}"
        role_ou="${role_info##*:}"
        CLIENT_NAME="${role_name}_client"
        if [ ! -f "certs/${CLIENT_NAME}.key" ]; then
            log "Creating certificate for: $CLIENT_NAME (OU=$role_ou)"
            $OPENSSL_CMD genrsa -out "certs/${CLIENT_NAME}.key" 2048 2>/dev/null
            $OPENSSL_CMD req -new \
                -key "certs/${CLIENT_NAME}.key" \
                -out "certs/${CLIENT_NAME}.csr" \
                -subj "/CN=${CLIENT_NAME}/O=IMS/OU=${role_ou}"
            $OPENSSL_CMD x509 -req \
                -in "certs/${CLIENT_NAME}.csr" \
                -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial \
                -out "certs/${CLIENT_NAME}.crt" -days 365 -sha256
        fi
    done

    rm -f certs/*.csr certs/*.srl 2>/dev/null

    # Default client cert = admin (for quick testing)
    cp certs/admin_client.crt certs/client.crt
    cp certs/admin_client.key certs/client.key

    log "Certificates ready in certs/"
}

# ============================================================================
# 2. Build server and client
# ============================================================================
build_all() {
    # Auto-detect cross-compiler for ARM (Ubuntu build host → RPi 4 target)
    CROSS_CC="aarch64-linux-gnu-gcc"
    if command -v "$CROSS_CC" &>/dev/null; then
        log "Building ARM ims_server and ARM LinuxRT tests (compiler: $CROSS_CC) ..."
        log "Building Linux-RT ARM server (ims_server)..."
        make all CC="$CROSS_CC" \
            EXTRA_CFLAGS="-I/usr/include/aarch64-linux-gnu" \
            EXTRA_LDFLAGS="-L/usr/lib/aarch64-linux-gnu"
    else
        log "Building native ims_server and ims_client ..."
        make all
    fi
    log "Build complete."
}

# ============================================================================
# 3. Deploy to RPi 4 via SCP
# ============================================================================
deploy() {
    local RPI_IP="$1"
    [ -z "$RPI_IP" ] && error "Usage: $0 deploy <RPi_IP>"
    [ -f "ims_server" ] || error "ims_server not found. Run 'make server' first."

    log "Deploying ims_server and certs to $RPI_IP ..."
    ssh "pi@${RPI_IP}" "mkdir -p ~/ims/certs"
    scp ims_server "pi@${RPI_IP}:~/ims/"
    scp certs/ca.crt certs/server.crt certs/server.key \
        "pi@${RPI_IP}:~/ims/certs/"

    log "Deployed. On the RPi 4 run:"
    echo "  ssh pi@${RPI_IP}"
    echo "  cd ~/ims && sudo ./ims_server"
}

# ============================================================================
# 4. Verify PREEMPT_RT on the RPi 4
# ============================================================================
rt_check() {
    local RPI_IP="$1"
    if [ -n "$RPI_IP" ]; then
        log "Checking PREEMPT_RT on $RPI_IP ..."
        ssh "pi@${RPI_IP}" "uname -a; cat /sys/kernel/realtime 2>/dev/null || echo 'not RT'"
    else
        log "Checking PREEMPT_RT on this machine ..."
        uname -a
        if [ -f /sys/kernel/realtime ] && [ "$(cat /sys/kernel/realtime)" = "1" ]; then
            log "PREEMPT_RT confirmed."
        else
            warn "This kernel does not appear to be PREEMPT_RT patched."
            warn "Install a real-time kernel:"
            warn "  sudo apt install linux-image-rt-arm64   (RPi OS / Debian)"
            warn "  sudo reboot"
        fi
    fi
}

# ============================================================================
# 5. Show Linux-RT kernel setup instructions
# ============================================================================
show_rt_instructions() {
    echo ""
    echo -e "${YELLOW}=== Linux-RT Kernel Setup (Raspberry Pi 4) ===${NC}"
    echo "Option A — Install pre-built RT kernel (easiest):"
    echo "  sudo apt update"
    echo "  sudo apt install linux-image-rt-arm64"
    echo "  sudo reboot"
    echo ""
    echo "Option B — Build PREEMPT_RT kernel from source:"
    echo "  1. Download kernel source: git clone --depth=1 https://github.com/raspberrypi/linux"
    echo "  2. Download PREEMPT_RT patch matching your kernel version from kernel.org"
    echo "  3. Apply patch: patch -p1 < rt-patch.patch"
    echo "  4. Configure: make bcm2711_defconfig && make menuconfig"
    echo "       -> General Setup -> Preemption Model -> Fully Preemptible Kernel (Real-Time)"
    echo "  5. Build and install: make -j4 && sudo make modules_install && sudo make install"
    echo "  6. Reboot and verify: uname -a  (should show PREEMPT_RT)"
    echo ""
    echo -e "${YELLOW}=== Enable I2C + 1-Wire on RPi OS ===${NC}"
    echo "Add to /boot/config.txt:"
    echo "  dtparam=i2c_arm=on"
    echo "  dtoverlay=w1-gpio,gpiopin=4    # adjust to your 1-Wire GPIO pin"
    echo ""
    echo "Install i2c tools:"
    echo "  sudo apt install i2c-tools"
    echo "  sudo i2cdetect -y 1            # verify ADS1115 appears at 0x48"
    echo ""
    echo -e "${GREEN}=== Server Usage ===${NC}"
    echo "  sudo ./ims_server              # root needed for SCHED_FIFO + GPIO"
    echo ""
    echo -e "${GREEN}=== Client Usage ===${NC}"
    echo "  ./ims_client <RPi_IP_ADDRESS>"
    echo ""
}

# ============================================================================
# Entry point
# ============================================================================
case "${1:-}" in
    certs)
        generate_certs
        ;;
    build)
        build_all
        ;;
    deploy)
        generate_certs
        build_all
        deploy "$2"
        ;;
    rt-check)
        rt_check "$2"
        ;;
    "")
        generate_certs
        build_all
        show_rt_instructions
        ;;
    *)
        error "Unknown option '$1'. Use: certs | build | deploy <IP> | rt-check [IP] | (empty)"
        ;;
esac

log "Done."
