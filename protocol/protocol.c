#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <openssl/ssl.h>

#include "protocol.h"
#include "authorization.h"
#include "sensor_manager.h"

#define EOM_MARKER '\x03'

/* ------------------------------------------------------------------ */
/*  TLS send / recv / poll helpers                                     */
/* ------------------------------------------------------------------ */
static inline int proto_send(ProtocolContext *ctx, const void *buf, int len) {
    return SSL_write(ctx->ssl, buf, len);
}

static inline int proto_recv(ProtocolContext *ctx, void *buf, int len) {
    return SSL_read(ctx->ssl, buf, len);
}

/*
 * proto_poll_readable: Returns > 0 if the socket has data, 0 on timeout
 * (1 second), < 0 on error.  Used by cmd_monitor to detect ENTER.
 */
static inline int proto_poll_readable(ProtocolContext *ctx) {
    int fd = SSL_get_fd(ctx->ssl);
    if (SSL_pending(ctx->ssl) > 0) return 1;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { 1, 0 };
    return select(fd + 1, &fds, NULL, NULL, &tv);
}

/* ------------------------------------------------------------------ */
/*  Black-box alert log  (file-based)                                 */
/* ------------------------------------------------------------------ */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void log_alert(const char *unit, const char *message) {
    pthread_mutex_lock(&log_mutex);
    FILE *f = fopen("blackbox.log", "a");
    if (f) {
        time_t now = time(NULL);
        char *ts = ctime(&now);
        if (ts) ts[strlen(ts) - 1] = '\0';  /* strip newline */
        fprintf(f, "[%s] CRITICAL ALERT | Unit: %s | %s\n",
                ts ? ts : "?", unit, message);
        fclose(f);
    }
    pthread_mutex_unlock(&log_mutex);
}

/* ------------------------------------------------------------------ */
/*  Role-based permission check                                        */
/* ------------------------------------------------------------------ */
static int role_can_execute(UserRole role, const char *command) {
    if (!command) return 0;

    /* Available to ALL roles */
    if (!strcmp(command, "help")        ||
        !strcmp(command, "whoami")      ||
        !strcmp(command, "list_units")  ||
        !strcmp(command, "get_sensors") ||
        !strcmp(command, "get_health")  ||
        !strcmp(command, "get_log")     ||
        !strcmp(command, "quit")        ||
        !strcmp(command, "exit"))
        return 1;

    /* monitor requires OPERATOR or above */
    if (!strcmp(command, "monitor"))
        return role == ROLE_OPERATOR    ||
               role == ROLE_MAINTENANCE ||
               role == ROLE_ADMIN;

    /* clear_log is ADMIN-only */
    if (!strcmp(command, "clear_log"))
        return role == ROLE_ADMIN;

    return 0;
}

static void send_permission_denied(ProtocolContext *ctx, const char *command) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Permission denied for role %s on command '%s'.\n",
             role_to_string(ctx->identity.role),
             command ? command : "unknown");
    send_response(ctx, msg);
    send_eom(ctx);
}

/* ------------------------------------------------------------------ */
/*  Protocol helpers                                                   */
/* ------------------------------------------------------------------ */
void send_response(ProtocolContext *ctx, const char *msg) {
    if (ctx && msg)
        proto_send(ctx, msg, (int)strlen(msg));
}

void send_eom(ProtocolContext *ctx) {
    char marker = EOM_MARKER;
    if (ctx)
        proto_send(ctx, &marker, 1);
}

/* ------------------------------------------------------------------ */
/*  Command handlers                                                   */
/* ------------------------------------------------------------------ */
void cmd_help(ProtocolContext *ctx) {
    send_response(ctx, "Available commands:\n");
    send_response(ctx, "  list_units     - List equipment\n");
    send_response(ctx, "  get_sensors    - Raw sensor readings\n");
    send_response(ctx, "  get_health     - Health status report\n");
    send_response(ctx, "  get_log        - Show blackbox log\n");
    if (ctx->identity.role == ROLE_OPERATOR  ||
        ctx->identity.role == ROLE_MAINTENANCE ||
        ctx->identity.role == ROLE_ADMIN)
        send_response(ctx, "  monitor [time] - Live telemetry stream\n");
    if (ctx->identity.role == ROLE_ADMIN)
        send_response(ctx, "  clear_log      - Wipe blackbox log\n");
    send_response(ctx, "  whoami         - Show identity\n");
    send_response(ctx, "  quit           - Disconnect\n");
    send_eom(ctx);
}

void cmd_whoami(ProtocolContext *ctx) {
    char buf[256];
    snprintf(buf, sizeof(buf), "User: %s | Role: %s\n",
             ctx->identity.common_name,
             role_to_string(ctx->identity.role));
    send_response(ctx, buf);
    send_eom(ctx);
}

void cmd_list_units(ProtocolContext *ctx) {
    char list[MAX_UNITS][MAX_ID_LENGTH];
    int count = manager_list_units(ctx->sensor_mgr, list, MAX_UNITS);
    send_response(ctx, "=== Registered Units ===\n");
    for (int i = 0; i < count; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), " - %s\n", list[i]);
        send_response(ctx, buf);
    }
    send_eom(ctx);
}

void cmd_get_sensors(ProtocolContext *ctx) {
    EquipmentHealth h;
    if (manager_get_health(ctx->sensor_mgr, "Sentinel-RT", &h)) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Vib: %.0f | Snd: %.1f%% | Temp: %.1fC | Cur: %.2fA\n",
                 h.snapshot.vibration_level,
                 h.snapshot.sound_level,
                 h.snapshot.temperature_c,
                 h.snapshot.current_a);
        send_response(ctx, buf);
    }
    send_eom(ctx);
}

void cmd_get_health(ProtocolContext *ctx) {
    EquipmentHealth h;
    if (manager_get_health(ctx->sensor_mgr, "Sentinel-RT", &h)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Status: %s | Message: %s\n",
                 health_to_string(h.status), h.message);
        send_response(ctx, buf);
    }
    send_eom(ctx);
}

void cmd_get_log(ProtocolContext *ctx) {
    FILE *f = fopen("blackbox.log", "r");
    if (!f) {
        send_response(ctx, "[INFO] Log is empty.\n");
        send_eom(ctx);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
        send_response(ctx, line);
    fclose(f);
    send_eom(ctx);
}

void cmd_clear_log(ProtocolContext *ctx) {
    FILE *f = fopen("blackbox.log", "w");
    if (f) fclose(f);
    send_response(ctx, "[SUCCESS] Log cleared.\n");
    send_eom(ctx);
}

void cmd_monitor(ProtocolContext *ctx, const char *args) {
    int max_ticks = -1;

    if (args && strlen(args) > 0) {
        int val;
        char unit = 's';
        if (sscanf(args, "%d%c", &val, &unit) >= 1) {
            if      (unit == 'm') max_ticks = val * 60;
            else if (unit == 'h') max_ticks = val * 3600;
            else                  max_ticks = val;
        }
    }

    char msg[128];
    if (max_ticks > 0)
        snprintf(msg, sizeof(msg), "\n>>> MONITOR START (Limit: %s) <<<\n", args);
    else
        snprintf(msg, sizeof(msg), "\n>>> MONITOR START (Infinite) <<<\n");
    send_response(ctx, msg);
    send_response(ctx, "Press ENTER to stop monitoring.\n\n");

    EquipmentHealth h;
    int ticks = 0;

    while (ctx->running) {
        if (manager_get_health(ctx->sensor_mgr, "Sentinel-RT", &h)) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "[%s] Vib: %.0f | Snd: %.0f%% | Temp: %.1fC | Cur: %.2fA\n",
                     health_to_string(h.status),
                     h.snapshot.vibration_level,
                     h.snapshot.sound_level,
                     h.snapshot.temperature_c,
                     h.snapshot.current_a);
            send_response(ctx, buf);

            if (h.status == HEALTH_CRITICAL)
                log_alert(h.unit_id, h.message);
        }

        if (proto_poll_readable(ctx) > 0) {
            char dummy;
            proto_recv(ctx, &dummy, 1);
            send_response(ctx, "\n>>> MONITOR STOPPED <<<\n");
            break;
        }

        if (max_ticks > 0 && ++ticks >= max_ticks) {
            send_response(ctx, "\n>>> MONITOR TIME LIMIT REACHED <<<\n");
            break;
        }
    }

    send_eom(ctx);
}

/* ------------------------------------------------------------------ */
/*  Protocol init and main loop                                        */
/* ------------------------------------------------------------------ */
void protocol_init(ProtocolContext *ctx, SSL *ssl,
                   ClientIdentity id, SensorManager *mgr) {
    ctx->ssl        = ssl;
    ctx->identity   = id;
    ctx->sensor_mgr = mgr;
    ctx->running    = 1;
}

void protocol_run(ProtocolContext *ctx) {
    char buf[1024];

    send_response(ctx, "--- Connected to Sentinel-RT Secure Server ---\n");
    send_eom(ctx);

    while (ctx->running) {
        int n = proto_recv(ctx, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';

        char command[64] = {0};
        if (sscanf(buf, "%63s", command) != 1)
            continue;

        if (!role_can_execute(ctx->identity.role, command)) {
            send_permission_denied(ctx, command);
            continue;
        }

        if      (!strcmp(command, "help"))        cmd_help(ctx);
        else if (!strcmp(command, "whoami"))       cmd_whoami(ctx);
        else if (!strcmp(command, "list_units"))   cmd_list_units(ctx);
        else if (!strcmp(command, "get_sensors"))  cmd_get_sensors(ctx);
        else if (!strcmp(command, "get_health"))   cmd_get_health(ctx);
        else if (!strcmp(command, "get_log"))      cmd_get_log(ctx);
        else if (!strcmp(command, "clear_log"))    cmd_clear_log(ctx);
        else if (!strcmp(command, "monitor")) {
            char *args = buf + strlen("monitor");
            while (*args == ' ' || *args == '\t') args++;
            cmd_monitor(ctx, args);
        }
        else if (!strcmp(command, "quit") || !strcmp(command, "exit")) {
            send_response(ctx, "\n>>> DISCONNECTING <<<\n");
            send_eom(ctx);
            ctx->running = 0;
            break;
        }
        else {
            send_response(ctx, "Unknown command. Type 'help'.\n");
            send_eom(ctx);
        }
    }
}
