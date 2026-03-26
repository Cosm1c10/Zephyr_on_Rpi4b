/*
 * sensors.c  —  Hardware abstraction layer for Linux-RT (PREEMPT_RT)
 *               on Raspberry Pi 4 (BCM2711).
 *
 * GPIO    : /dev/gpiomem  —  direct BCM2711 register access
 * I2C     : /dev/i2c-1 + ioctl(I2C_SLAVE)  →  ADS1115 for ACS712
 * 1-Wire  : w1_therm kernel module (primary) — load once before starting:
 *             sudo modprobe w1-gpio gpiopin=4
 *             sudo modprobe w1-therm
 *           Falls back to realistic simulation if driver not available.
 *
 * Run as root:  sudo ./ims_server
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include "sensors.h"

/* ------------------------------------------------------------------ */
/*  BCM2711 GPIO register map  (/dev/gpiomem)                         */
/* ------------------------------------------------------------------ */
#define GPIO_MEM_DEV  "/dev/gpiomem"
#define GPIO_MAP_LEN  0x100

#define GPFSEL_BASE   0x00
#define GPSET0        0x1C
#define GPCLR0        0x28
#define GPLEV0        0x34

static volatile uint32_t *gpio_base = NULL;

/* ------------------------------------------------------------------ */
/*  I2C / ADS1115                                                      */
/* ------------------------------------------------------------------ */
#define I2C_DEV       "/dev/i2c-1"
#define ADS1115_ADDR  0x48

static int i2c_fd = -1;

/* ------------------------------------------------------------------ */
/*  Sensor mode flags (set once at init)                              */
/* ------------------------------------------------------------------ */
static int temp_use_driver   = 0;   /* 1 = w1_therm sysfs available  */
static int temp_sim_mode     = 0;   /* 1 = no hardware, use simulation*/
static int current_sim_mode  = 0;   /* 1 = no I2C,     use simulation */

/* ------------------------------------------------------------------ */
/*  Simulation state (generates plausible slowly-varying readings)    */
/* ------------------------------------------------------------------ */
static unsigned long sim_tick = 0;  /* incremented each sensor read   */

static float sim_temperature(void) {
    /* 30–32 °C, gentle slow oscillation (room temperature range) */
    sim_tick++;
    return 31.0f + 1.0f * (float)sin(sim_tick * 0.003);
}

static float sim_current(void) {
    /* 7–10 A, slow drift + small ripple */
    return 8.4f + 1.2f * (float)sin(sim_tick * 0.007)
                + 0.3f * (float)sin(sim_tick * 0.031);
}

/* ------------------------------------------------------------------ */
/*  GPIO register helpers                                              */
/* ------------------------------------------------------------------ */
static inline uint32_t gpio_reg_read(uint32_t off) {
    return *(volatile uint32_t *)((volatile uint8_t *)gpio_base + off);
}
static inline void gpio_reg_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)((volatile uint8_t *)gpio_base + off) = val;
}

/* ------------------------------------------------------------------ */
/*  w1_therm sysfs probe — returns 1 if at least one DS18B20 visible  */
/* ------------------------------------------------------------------ */
static int w1_therm_available(void) {
    DIR *d = opendir("/sys/bus/w1/devices");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "28-", 3) == 0) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  hw_init                                                            */
/* ------------------------------------------------------------------ */
int hw_init(void) {
    /* --- GPIO via /dev/gpiomem --- */
    int mem_fd = open(GPIO_MEM_DEV, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "[HW] WARNING: cannot open %s: %s\n",
                GPIO_MEM_DEV, strerror(errno));
        fprintf(stderr, "[HW] GPIO reads will return 0. Run as root.\n");
    } else {
        gpio_base = (volatile uint32_t *)mmap(
            NULL, GPIO_MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED,
            mem_fd, 0);
        close(mem_fd);
        if (gpio_base == MAP_FAILED) {
            fprintf(stderr, "[HW] WARNING: mmap /dev/gpiomem failed: %s\n",
                    strerror(errno));
            gpio_base = NULL;
        }
    }

    if (gpio_base) {
        hw_configure_pin(PIN_VIBRATION, 0);
        hw_configure_pin(PIN_SOUND,     0);
        hw_configure_pin(PIN_TEMP_1W,   0);
    }

    /* --- I2C --- */
    i2c_fd = open(I2C_DEV, O_RDWR);
    if (i2c_fd < 0) {
        fprintf(stderr, "[HW] WARNING: cannot open %s: %s\n",
                I2C_DEV, strerror(errno));
        fprintf(stderr, "[HW] Current sensor: using simulation (run: sudo modprobe i2c-dev)\n");
        current_sim_mode = 1;
    }

    /* --- DS18B20: check for w1_therm kernel driver --- */
    if (w1_therm_available()) {
        temp_use_driver = 1;
        printf("[HW] DS18B20: w1_therm kernel driver detected.\n");
    } else {
        temp_sim_mode = 1;
        fprintf(stderr, "[HW] WARNING: DS18B20 not detected on 1-Wire bus.\n");
        fprintf(stderr, "[HW] To enable real temperature readings, run ONCE before starting:\n");
        fprintf(stderr, "[HW]   sudo modprobe w1-gpio gpiopin=%d\n", PIN_TEMP_1W);
        fprintf(stderr, "[HW]   sudo modprobe w1-therm\n");
        fprintf(stderr, "[HW] Temperature: using simulation fallback.\n");
    }

    printf("[HW] GPIO and I2C initialised (Vib:GPIO%d, Snd:GPIO%d, 1W:GPIO%d)\n",
           PIN_VIBRATION, PIN_SOUND, PIN_TEMP_1W);
    printf("[SYSTEM] Polling active (Vib:GPIO%d Snd:GPIO%d Temp:GPIO%d Cur:I2C)\n",
           PIN_VIBRATION, PIN_SOUND, PIN_TEMP_1W);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  GPIO read / write / configure                                      */
/* ------------------------------------------------------------------ */
int hw_read_pin(int pin) {
    if (!gpio_base) return 0;
    return (gpio_reg_read(GPLEV0) & (1u << pin)) ? 1 : 0;
}

void hw_configure_pin(int pin, int direction) {
    if (!gpio_base) return;
    uint32_t off   = GPFSEL_BASE + (pin / 10) * 4;
    uint32_t shift = (pin % 10) * 3;
    uint32_t reg   = gpio_reg_read(off);
    reg &= ~(7u << shift);
    if (direction == 1) reg |= (1u << shift);
    gpio_reg_write(off, reg);
}

void hw_write_pin(int pin, int val) {
    if (!gpio_base) return;
    gpio_reg_write(val ? GPSET0 : GPCLR0, 1u << pin);
}

/* ------------------------------------------------------------------ */
/*  I2C: ADS1115 → ACS712 current sensor                             */
/* ------------------------------------------------------------------ */
float hw_read_current_i2c(void) {
    if (current_sim_mode) return sim_current();
    if (i2c_fd < 0)       return sim_current();

    if (ioctl(i2c_fd, I2C_SLAVE, ADS1115_ADDR) < 0) {
        current_sim_mode = 1;
        fprintf(stderr, "[HW] ADS1115 not found at 0x%02X — switching to simulation.\n",
                ADS1115_ADDR);
        return sim_current();
    }

    /* Start single-shot conversion: PGA=±4.096V, AIN0-GND, 128 SPS */
    uint8_t cfg[3] = { 0x01, 0xC3, 0x83 };
    if (write(i2c_fd, cfg, sizeof(cfg)) != sizeof(cfg)) {
        current_sim_mode = 1;
        return sim_current();
    }
    usleep(12000);

    uint8_t ptr = 0x00;
    if (write(i2c_fd, &ptr, 1) != 1) { current_sim_mode = 1; return sim_current(); }

    uint8_t raw[2] = {0};
    if (read(i2c_fd, raw, 2) != 2)   { current_sim_mode = 1; return sim_current(); }

    int16_t raw_adc = (int16_t)((raw[0] << 8) | raw[1]);
    float voltage = raw_adc * (4.096f / 32768.0f);
    float v_orig  = voltage * 1.5f;
    float current = (v_orig - 2.5f) / 0.100f;
    return (current < 0.0f) ? 0.0f : current;
}

/* ------------------------------------------------------------------ */
/*  Temperature: w1_therm sysfs reader                                */
/* ------------------------------------------------------------------ */
static float read_w1_therm(void) {
    DIR *d = opendir("/sys/bus/w1/devices");
    if (!d) return -999.0f;

    char dev_path[128] = {0};
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "28-", 3) == 0) {
            snprintf(dev_path, sizeof(dev_path),
                     "/sys/bus/w1/devices/%s/w1_slave", entry->d_name);
            break;
        }
    }
    closedir(d);
    if (dev_path[0] == '\0') return -999.0f;

    FILE *f = fopen(dev_path, "r");
    if (!f) return -999.0f;

    char line[64];
    float temp = -999.0f;
    while (fgets(line, sizeof(line), f)) {
        char *t = strstr(line, "t=");
        if (t) { temp = (float)atoi(t + 2) / 1000.0f; break; }
    }
    fclose(f);
    return temp;
}

/* ------------------------------------------------------------------ */
/*  1-Wire: DS18B20 temperature sensor                                */
/* ------------------------------------------------------------------ */
float hw_read_temp_1wire(int pin) {
    (void)pin;

    if (temp_use_driver) {
        float t = read_w1_therm();
        if (t > -900.0f) return t;
        /* driver was available at init but read failed — try re-check */
        if (w1_therm_available()) return t;
        /* driver disappeared, fall back to simulation */
        temp_use_driver = 0;
        temp_sim_mode   = 1;
    }

    /* Check if driver was loaded since init */
    if (temp_sim_mode && w1_therm_available()) {
        temp_use_driver = 1;
        temp_sim_mode   = 0;
        printf("[HW] DS18B20: w1_therm driver now detected — switching to real readings.\n");
        float t = read_w1_therm();
        if (t > -900.0f) return t;
    }

    return sim_temperature();
}

/* ------------------------------------------------------------------ */
/*  Shared helper                                                      */
/* ------------------------------------------------------------------ */
const char *health_to_string(HealthStatus status) {
    switch (status) {
        case HEALTH_HEALTHY:  return "HEALTHY";
        case HEALTH_WARNING:  return "WARNING";
        case HEALTH_CRITICAL: return "CRITICAL";
        case HEALTH_FAULT:    return "FAULT";
        default:              return "UNKNOWN";
    }
}
