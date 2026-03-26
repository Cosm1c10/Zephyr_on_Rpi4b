/*
 * sensors.c  —  Hardware abstraction layer for Linux-RT (PREEMPT_RT)
 *               on Raspberry Pi 4 (BCM2711).
 *
 * GPIO  : /dev/gpiomem  —  direct BCM2711 register access (no sysfs/overlay needed)
 * I2C   : /dev/i2c-1 + ioctl(I2C_SLAVE)  →  ADS1115 ADC for ACS712
 * 1-Wire: bit-bang via GPIO on PIN_TEMP_1W  →  DS18B20 (no w1_therm driver needed)
 *
 * No /boot/config.txt changes required.
 * Run as root (or add user to gpio/i2c groups).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include "sensors.h"

/* ------------------------------------------------------------------ */
/*  BCM2711 GPIO register map (via /dev/gpiomem)                      */
/* ------------------------------------------------------------------ */
#define GPIO_MEM_DEV   "/dev/gpiomem"
#define GPIO_MAP_LEN   0x100

/* Register offsets (byte offsets, each reg is 32-bit) */
#define GPFSEL_BASE    0x00   /* Function select: GPFSEL0..5 */
#define GPSET0         0x1C   /* Pin output set  (pins 0-31) */
#define GPCLR0         0x28   /* Pin output clear (pins 0-31) */
#define GPLEV0         0x34   /* Pin level read  (pins 0-31) */

static volatile uint32_t *gpio_base = NULL;

/* ------------------------------------------------------------------ */
/*  DS18B20 commands                                                   */
/* ------------------------------------------------------------------ */
#define DS18X20_CMD_SKIP_ROM        0xCC
#define DS18X20_CMD_CONVERT_T       0x44
#define DS18X20_CMD_READ_SCRATCHPAD 0xBE

/* ------------------------------------------------------------------ */
/*  I2C device                                                         */
/* ------------------------------------------------------------------ */
#define I2C_DEV       "/dev/i2c-1"
#define ADS1115_ADDR  0x48

static int i2c_fd = -1;
static int temp_warned = 0;

/* ------------------------------------------------------------------ */
/*  Low-level GPIO register helpers  (mirrors QNX in32/out32)         */
/* ------------------------------------------------------------------ */
static inline uint32_t gpio_reg_read(uint32_t offset_bytes) {
    return *(volatile uint32_t *)((volatile uint8_t *)gpio_base + offset_bytes);
}
static inline void gpio_reg_write(uint32_t offset_bytes, uint32_t val) {
    *(volatile uint32_t *)((volatile uint8_t *)gpio_base + offset_bytes) = val;
}

/* ------------------------------------------------------------------ */
/*  hw_init                                                            */
/* ------------------------------------------------------------------ */
int hw_init(void) {
    /* Map BCM2711 GPIO registers */
    int mem_fd = open(GPIO_MEM_DEV, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "[HW] ERROR: cannot open %s: %s\n",
                GPIO_MEM_DEV, strerror(errno));
        fprintf(stderr, "[HW] Run as root or add user to 'gpio' group.\n");
        return -1;
    }
    gpio_base = (volatile uint32_t *)mmap(
        NULL, GPIO_MAP_LEN,
        PROT_READ | PROT_WRITE, MAP_SHARED,
        mem_fd, 0);
    close(mem_fd);

    if (gpio_base == MAP_FAILED) {
        fprintf(stderr, "[HW] ERROR: mmap /dev/gpiomem failed: %s\n",
                strerror(errno));
        gpio_base = NULL;
        return -1;
    }

    /* Configure Vibration and Sound pins as inputs */
    hw_configure_pin(PIN_VIBRATION, 0);
    hw_configure_pin(PIN_SOUND,     0);
    /* 1-Wire pin starts as input (idle, pulled high externally) */
    hw_configure_pin(PIN_TEMP_1W,   0);

    /* Open I2C bus */
    i2c_fd = open(I2C_DEV, O_RDWR);
    if (i2c_fd < 0) {
        fprintf(stderr, "[HW] WARNING: cannot open %s: %s\n",
                I2C_DEV, strerror(errno));
        fprintf(stderr, "[HW] Current sensor will return 0.0 A\n");
    }

    printf("[HW] GPIO (/dev/gpiomem) and I2C initialised "
           "(Vib:GPIO%d, Snd:GPIO%d, 1W:GPIO%d)\n",
           PIN_VIBRATION, PIN_SOUND, PIN_TEMP_1W);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  GPIO read / write / configure  (same logic as QNX version)        */
/* ------------------------------------------------------------------ */
int hw_read_pin(int pin) {
    if (!gpio_base) return 0;
    uint32_t level = gpio_reg_read(GPLEV0);
    return (level & (1u << pin)) ? 1 : 0;
}

void hw_configure_pin(int pin, int direction) {
    if (!gpio_base) return;
    uint32_t fsel_offset = GPFSEL_BASE + (pin / 10) * 4;
    uint32_t shift       = (pin % 10) * 3;
    uint32_t reg         = gpio_reg_read(fsel_offset);
    reg &= ~(7u << shift);           /* clear 3 function bits */
    if (direction == 1)
        reg |= (1u << shift);        /* 001 = output */
    /* 000 = input (default after clear) */
    gpio_reg_write(fsel_offset, reg);
}

void hw_write_pin(int pin, int val) {
    if (!gpio_base) return;
    if (val)
        gpio_reg_write(GPSET0, 1u << pin);
    else
        gpio_reg_write(GPCLR0, 1u << pin);
}

/* ------------------------------------------------------------------ */
/*  I2C: ADS1115 → ACS712 current sensor                             */
/* ------------------------------------------------------------------ */
float hw_read_current_i2c(void) {
    if (i2c_fd < 0) return 0.0f;

    if (ioctl(i2c_fd, I2C_SLAVE, ADS1115_ADDR) < 0) return 0.0f;

    /* Write config register: pointer=0x01, MSB=0xC3, LSB=0x83
     * PGA=±4.096V, AIN0-GND, single-shot, 128 SPS              */
    uint8_t cfg[3] = { 0x01, 0xC3, 0x83 };
    if (write(i2c_fd, cfg, sizeof(cfg)) != sizeof(cfg)) return 0.0f;

    usleep(12000);  /* wait for single-shot conversion (~10 ms) */

    uint8_t ptr = 0x00;
    if (write(i2c_fd, &ptr, 1) != 1) return 0.0f;

    uint8_t raw[2] = {0};
    if (read(i2c_fd, raw, 2) != 2) return 0.0f;

    int16_t raw_adc = (int16_t)((raw[0] << 8) | raw[1]);
    float   voltage = raw_adc * (4.096f / 32768.0f);
    float   v_orig  = voltage * 1.5f;   /* voltage-divider compensation */
    float   current = (v_orig - 2.5f) / 0.100f;

    return (current < 0.0f) ? 0.0f : current;
}

/* ------------------------------------------------------------------ */
/*  1-Wire bit-bang helpers  (identical logic to QNX version)         */
/* ------------------------------------------------------------------ */
static void ow_drive_low(int pin) {
    hw_configure_pin(pin, 1);   /* output */
    hw_write_pin(pin, 0);
}

static void ow_release(int pin) {
    hw_configure_pin(pin, 0);   /* input — bus pulled high by 4.7k */
}

static int ow_reset(int pin) {
    ow_drive_low(pin);
    usleep(480);
    ow_release(pin);
    usleep(70);
    int presence = (hw_read_pin(pin) == 0);
    usleep(410);
    return presence;
}

static void ow_write_bit(int pin, int bit) {
    ow_drive_low(pin);
    if (bit) {
        usleep(6);
        ow_release(pin);
        usleep(64);
    } else {
        usleep(60);
        ow_release(pin);
        usleep(10);
    }
}

static int ow_read_bit(int pin) {
    ow_drive_low(pin);
    usleep(6);
    ow_release(pin);
    usleep(9);
    int bit = hw_read_pin(pin) ? 1 : 0;
    usleep(55);
    return bit;
}

static void ow_write_byte(int pin, uint8_t value) {
    for (int i = 0; i < 8; i++)
        ow_write_bit(pin, (value >> i) & 0x01);
}

static uint8_t ow_read_byte(int pin) {
    uint8_t value = 0;
    for (int i = 0; i < 8; i++)
        value |= (uint8_t)(ow_read_bit(pin) << i);
    return value;
}

static uint8_t ds18b20_crc8(const uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t in = data[i];
        for (int b = 0; b < 8; b++) {
            uint8_t mix = (crc ^ in) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            in >>= 1;
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/*  1-Wire: DS18B20 temperature sensor                                */
/* ------------------------------------------------------------------ */
float hw_read_temp_1wire(int pin) {
    uint8_t scratch[9] = {0};

    if (!gpio_base) {
        if (!temp_warned) {
            fprintf(stderr, "[HW] WARNING: GPIO not initialised for DS18B20.\n");
            temp_warned = 1;
        }
        return 0.0f;
    }

    if (!ow_reset(pin)) {
        if (!temp_warned) {
            fprintf(stderr, "[HW] WARNING: DS18B20 not detected on GPIO%d. "
                    "Check wiring and 4.7k pull-up resistor.\n", pin);
            temp_warned = 1;
        }
        return 0.0f;
    }

    /* Trigger temperature conversion */
    ow_write_byte(pin, DS18X20_CMD_SKIP_ROM);
    ow_write_byte(pin, DS18X20_CMD_CONVERT_T);

    /* Poll for conversion complete (up to 800 ms) */
    int ready = 0;
    for (int i = 0; i < 800; i++) {
        if (ow_read_bit(pin)) { ready = 1; break; }
        usleep(1000);
    }
    if (!ready) {
        if (!temp_warned) {
            fprintf(stderr, "[HW] WARNING: DS18B20 conversion timeout.\n");
            temp_warned = 1;
        }
        return 0.0f;
    }

    if (!ow_reset(pin)) return 0.0f;

    /* Read scratchpad and validate CRC */
    ow_write_byte(pin, DS18X20_CMD_SKIP_ROM);
    ow_write_byte(pin, DS18X20_CMD_READ_SCRATCHPAD);
    for (int i = 0; i < 9; i++) scratch[i] = ow_read_byte(pin);

    if (ds18b20_crc8(scratch, 8) != scratch[8]) {
        if (!temp_warned) {
            fprintf(stderr, "[HW] WARNING: DS18B20 CRC mismatch.\n");
            temp_warned = 1;
        }
        return 0.0f;
    }

    temp_warned = 0;
    int16_t raw = (int16_t)((scratch[1] << 8) | scratch[0]);
    return (float)raw / 16.0f;
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
