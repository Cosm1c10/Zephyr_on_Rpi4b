#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "sensor_manager.h"
#include "sensors.h"

// ============================================================
// INTERNAL STATE & THREADING
// ============================================================
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
static EquipmentHealth current_health;
static int running = 0;

// Thresholds
#define VIB_WARNING_THRESHOLD  100.0
#define VIB_CRITICAL_THRESHOLD 200.0
#define TMP_CRITICAL_THRESHOLD 80.0
#define CUR_CRITICAL_THRESHOLD 15.0

// ============================================================
// POLLING PARAMETERS
// ============================================================
#define POLL_INTERVAL_US   1000   /* 1ms GPIO poll tick                   */
#define AGGREGATION_TICKS  250    /* 250ms window → 4 updates per second  */

/* Sound debounce: require 5 consecutive HIGH ticks (5ms) to register one
 * event, then enforce a 50ms cooldown before the next event is counted.
 * This filters electrical noise and brief glitches.                      */
#define SND_DEBOUNCE_TICKS 5
#define SND_COOLDOWN_TICKS 50

// ============================================================
// BACKGROUND POLLING THREAD (1kHz GPIO, 4Hz snapshot)
// ============================================================
void* polling_thread(void* arg) {
    (void)arg;
    uint32_t vib_counts    = 0;
    uint32_t snd_counts    = 0;
    int      poll_tick     = 0;
    int      snd_debounce  = 0;   /* consecutive HIGH ticks        */
    int      snd_cooldown  = 0;   /* ticks remaining in cooldown   */

    printf("[SENSORS] Background polling thread started.\n");

    while (running) {
        /* --- Vibration: count every HIGH tick --- */
        if (hw_read_pin(PIN_VIBRATION)) vib_counts++;

        /* --- Sound: debounce + cooldown filter --- */
        if (snd_cooldown > 0) {
            snd_cooldown--;
        } else if (hw_read_pin(PIN_SOUND)) {
            snd_debounce++;
            if (snd_debounce >= SND_DEBOUNCE_TICKS) {
                snd_counts++;
                snd_debounce = 0;
                snd_cooldown = SND_COOLDOWN_TICKS;
            }
        } else {
            snd_debounce = 0;
        }

        usleep(POLL_INTERVAL_US);
        poll_tick++;

        /* --- Aggregate and publish every 250ms --- */
        if (poll_tick >= AGGREGATION_TICKS) {
            pthread_mutex_lock(&data_mutex);

            /* Scale vibration to events/second (window = 250ms = 0.25s) */
            current_health.snapshot.vibration_level =
                (float)vib_counts * (1000.0f / AGGREGATION_TICKS);

            /* Sound: each count is a verified event; cap display at 100% */
            current_health.snapshot.sound_level =
                (snd_counts > 4) ? 100.0f : (float)snd_counts * 25.0f;

            current_health.snapshot.temperature_c = hw_read_temp_1wire(PIN_TEMP_1W);
            current_health.snapshot.current_a     = hw_read_current_i2c();

            /* Evaluate Health Status */
            if (current_health.snapshot.vibration_level > VIB_CRITICAL_THRESHOLD ||
                current_health.snapshot.current_a > CUR_CRITICAL_THRESHOLD) {
                current_health.status = HEALTH_CRITICAL;
                strcpy(current_health.message, "CRITICAL FAULT DETECTED");
            } else if (current_health.snapshot.vibration_level > VIB_WARNING_THRESHOLD) {
                current_health.status = HEALTH_WARNING;
                strcpy(current_health.message, "High Vibration Warning");
            } else {
                current_health.status = HEALTH_HEALTHY;
                strcpy(current_health.message, "System Nominal");
            }

            pthread_mutex_unlock(&data_mutex);

            vib_counts   = 0;
            snd_counts   = 0;
            snd_debounce = 0;
            poll_tick    = 0;
        }
    }
    return NULL;
}

// ============================================================
// PUBLIC API
// ============================================================

int manager_init(SensorManager* mgr) {
    if (hw_init() != 0) return -1;

    // Initialize state
    memset(&current_health, 0, sizeof(EquipmentHealth));
    strcpy(current_health.unit_id, "Sentinel-RT");
    
    running = 1;
    mgr->is_running = 1;

    // Start background thread
    if (pthread_create(&mgr->thread_id, NULL, polling_thread, mgr) != 0) {
        perror("pthread_create failed");
        return -1;
    }

    return 0; // Success
}

int manager_get_health(SensorManager* mgr, const char* unit_id, EquipmentHealth* out_health) {
    (void)mgr;
    pthread_mutex_lock(&data_mutex);
    
    // Only return data if IDs match
    if (strcmp(current_health.unit_id, unit_id) == 0) {
        memcpy(out_health, &current_health, sizeof(EquipmentHealth));
        pthread_mutex_unlock(&data_mutex);
        return 1;
    }
    
    pthread_mutex_unlock(&data_mutex);
    return 0;
}

int manager_list_units(SensorManager* mgr, char list[MAX_UNITS][MAX_ID_LENGTH], int max_units) {
    (void)mgr; (void)max_units;
    strcpy(list[0], current_health.unit_id);
    return 1;
}

void manager_cleanup(SensorManager* mgr) {
    if (running) {
        running = 0;
        mgr->is_running = 0;
        pthread_join(mgr->thread_id, NULL);
        printf("[SENSORS] Background thread joined and cleaned up successfully.\n");
    }
}
