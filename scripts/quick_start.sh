#!/bin/bash

# ============================================================================
# IMS Quick Start Script
# ============================================================================

# Ensure we run from the project root (one level up from scripts/)
cd "$(dirname "$0")/.."

# Configuration
SERVER_IP="10.42.0.171"
QNX_USER="qnxuser"
REMOTE_DIR="/home/qnxuser/ims"

# FORCE SYSTEM OPENSSL
OPENSSL_CMD="/usr/bin/openssl"
export OPENSSL_CONF="/etc/ssl/openssl.cnf"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

log() { echo -e "${GREEN}[INFO] $1${NC}"; }
error() { echo -e "${RED}[ERROR] $1${NC}"; exit 1; }

# 1. Generate Certificates
generate_certs() {
    log "Generating SSL Certificates in certs/ folder..."
    mkdir -p certs
    
    if [ ! -f "$OPENSSL_CMD" ]; then OPENSSL_CMD="openssl"; fi

    # CA
    if [ ! -f "certs/ca.key" ]; then
        $OPENSSL_CMD genrsa -out certs/ca.key 2048 2>/dev/null
        $OPENSSL_CMD req -x509 -new -nodes -key certs/ca.key -sha256 -days 365 -out certs/ca.crt -subj "/CN=IMS_Root_CA"
    fi

    # Server
    if [ ! -f "certs/server.key" ]; then
        $OPENSSL_CMD genrsa -out certs/server.key 2048 2>/dev/null
        $OPENSSL_CMD req -new -key certs/server.key -out certs/server.csr -subj "/CN=ims_server"
        $OPENSSL_CMD x509 -req -in certs/server.csr -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial -out certs/server.crt -days 365 -sha256
    fi

    # Clients
    ROLES=("admin:ADMIN" "operator:OPERATOR" "viewer:VIEWER" "maintenance:MAINTENANCE")
    for role_info in "${ROLES[@]}"; do
        role_name="${role_info%%:*}"
        role_ou="${role_info##*:}"
        CLIENT_NAME="${role_name}_client"
        if [ ! -f "certs/${CLIENT_NAME}.key" ]; then
            log "Creating certificate for: $CLIENT_NAME"
            $OPENSSL_CMD genrsa -out "certs/${CLIENT_NAME}.key" 2048 2>/dev/null
            $OPENSSL_CMD req -new -key "certs/${CLIENT_NAME}.key" -out "certs/${CLIENT_NAME}.csr" -subj "/CN=${CLIENT_NAME}/O=IMS/OU=${role_ou}"
            $OPENSSL_CMD x509 -req -in "certs/${CLIENT_NAME}.csr" -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial -out "certs/${CLIENT_NAME}.crt" -days 365 -sha256
        fi
    done

    # Clean up intermediate CSRs and serial files
    rm certs/*.csr certs/*.srl 2>/dev/null

    # Default Client (Admin)
    cp certs/admin_client.crt certs/client.crt
    cp certs/admin_client.key certs/client.key
}

# 2. Build
build_project() {
    log "Building Project..."
    make clean
    make all
    if [ $? -ne 0 ]; then error "Build Failed"; fi
    log "Build Successful!"
}

# 3. Deploy
deploy() {
    log "Deploying to QNX ($SERVER_IP)..."
    ssh $QNX_USER@$SERVER_IP "slay ims_server" 2>/dev/null
    
    # Create main directory and certs subdirectory remotely
    ssh $QNX_USER@$SERVER_IP "mkdir -p $REMOTE_DIR/certs"
    
    if [ -f "ims_server" ] && [ -f "certs/server.crt" ]; then
        # Transfer Binaries to root remote dir
        scp ims_server sensor_test $QNX_USER@$SERVER_IP:$REMOTE_DIR/
        if [ $? -ne 0 ]; then error "Binary deployment failed."; fi
        
        # Transfer Server Certs to remote certs/ dir
        scp certs/server.crt certs/server.key certs/ca.crt $QNX_USER@$SERVER_IP:$REMOTE_DIR/certs/
        if [ $? -ne 0 ]; then error "Certificate deployment failed."; fi
        
        log "Deployment Complete!"
    else
        error "Files missing. Build or certificate generation failed."
    fi
}

# Execution
generate_certs
build_project
if [ "$1" == "deploy" ] || [ "$1" == "all" ]; then deploy; fi

log "Done."
