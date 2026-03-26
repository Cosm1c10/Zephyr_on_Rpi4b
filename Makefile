# ============================================================================
# IMS Project Makefile  —  Linux-RT (PREEMPT_RT) / Raspberry Pi 4
#
# Targets:
#   make              → build server + client
#   make server       → build ims_server (runs on the RPi 4)
#   make client       → build ims_client (runs on any Linux host)
#   make certs        → generate TLS certificates
#   make clean        → remove binaries
#
# Prerequisites (on the RPi 4 / build host):
#   sudo apt install gcc libssl-dev
#
# Cross-compile for RPi 4 (from x86-64 host):
#   CC=aarch64-linux-gnu-gcc make
#
# Running the server on the RPi 4:
#   sudo ./ims_server      # root required for SCHED_FIFO + GPIO sysfs
#
# Verify PREEMPT_RT kernel:
#   uname -a              # should show "PREEMPT_RT" in the kernel string
#   cat /sys/kernel/realtime   # should print "1"
# ============================================================================

CC = gcc

INCLUDES     = -I./apps -I./common -I./drivers -I./protocol
CFLAGS       = -D_GNU_SOURCE -Wall -Wextra -O2 $(INCLUDES)

LIBS_SERVER  = -lssl -lcrypto -lpthread -lrt -lm
LIBS_CLIENT  = -lssl -lcrypto -lpthread

SRC_SERVER = apps/server.c \
             common/authorization.c \
             drivers/sensors.c \
             drivers/sensor_manager.c \
             protocol/protocol.c

SRC_CLIENT = apps/client.c

TARGET_SERVER = ims_server
TARGET_CLIENT = ims_client

# ============================================================================
# Build targets
# ============================================================================

all: server client

server:
	@echo "[INFO] Building Linux-RT server (ims_server)..."
	$(CC) $(CFLAGS) -o $(TARGET_SERVER) $(SRC_SERVER) $(LIBS_SERVER)
	@echo "[OK]  $(TARGET_SERVER) built."

client:
	@echo "[INFO] Building Linux client (ims_client)..."
	$(CC) $(CFLAGS) -o $(TARGET_CLIENT) $(SRC_CLIENT) $(LIBS_CLIENT)
	@echo "[OK]  $(TARGET_CLIENT) built."

certs:
	@./scripts/quick_start.sh certs

clean:
	@echo "[INFO] Cleaning up..."
	rm -f $(TARGET_SERVER) $(TARGET_CLIENT) *.o blackbox.log
	@echo "[DONE]"

.PHONY: all server client certs clean
