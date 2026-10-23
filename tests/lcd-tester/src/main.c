/* LCD tester: scans the I2C bus, then drives a 1602 PCF8574 LCD found on 0x27 or 0x3f. */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <stdio.h>

LOG_MODULE_REGISTER(lcd_tester, LOG_LEVEL_INF);

#define LCD_COLS 16
#define LCD_RS 0x01
#define LCD_EN 0x04
#define LCD_BACKLIGHT 0x08

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static uint16_t lcd_addr;
static uint8_t backlight = LCD_BACKLIGHT;

static int expander_write(uint8_t value)
{
    return i2c_write(i2c, &value, 1, lcd_addr);
}

static int write_nibble(uint8_t nibble, bool rs)
{
    uint8_t value = (uint8_t)((nibble & 0x0f) << 4) | backlight | (rs ? LCD_RS : 0);
    int rc = expander_write(value | LCD_EN);
    if (rc < 0) return rc;
    k_busy_wait(1);
    rc = expander_write(value & (uint8_t)~LCD_EN);
    k_busy_wait(50);
    return rc;
}

static int send(uint8_t value, bool rs)
{
    int rc = write_nibble(value >> 4, rs);
    return rc < 0 ? rc : write_nibble(value & 0x0f, rs);
}

static int command(uint8_t cmd)
{
    int rc = send(cmd, false);
    if (cmd == 0x01 || cmd == 0x02) k_msleep(2);
    return rc;
}

static int lcd_init(void)
{
    k_msleep(50);
    int rc = 0;
    for (int i = 0; i < 3 && rc == 0; i++) {
        rc = write_nibble(0x03, false);
        k_msleep(5);
    }
    if (rc == 0) rc = write_nibble(0x02, false);
    if (rc == 0) rc = command(0x28);
    if (rc == 0) rc = command(0x08);
    if (rc == 0) rc = command(0x01);
    if (rc == 0) rc = command(0x06);
    if (rc == 0) rc = command(0x0c);
    return rc;
}

static void print_at(uint8_t col, uint8_t row, const char *text)
{
    command(0x80 | ((row ? 0x40 : 0x00) + col));
    for (; *text && col < LCD_COLS; text++, col++) send((uint8_t)*text, true);
}

static uint16_t scan_bus(void)
{
    uint16_t found = 0;
    uint8_t dummy;

    LOG_INF("Scanning I2C bus...");
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_read(i2c, &dummy, 1, addr) == 0) {
            LOG_INF("  device at 0x%02x", addr);
            if (!found && (addr == 0x27 || addr == 0x3f)) found = addr;
        }
    }
    return found;
}

int main(void)
{
    if (!device_is_ready(i2c)) {
        LOG_ERR("i2c0 not ready");
        return 0;
    }

    for (;;) {
        lcd_addr = scan_bus();
        if (lcd_addr && lcd_init() == 0) break;
        LOG_ERR("No LCD at 0x27/0x3f (check wiring: SDA P0.26, SCL P0.27, 5 V, GND). Retrying in 3 s");
        k_msleep(3000);
    }
    LOG_INF("LCD found at 0x%02x", lcd_addr);

    print_at(0, 0, "HALLOWEEN PUZZLE");
    print_at(0, 1, "LCD test OK");
    k_msleep(3000);

    /* Full-width characters show contrast problems and any dead cells. */
    command(0x01);
    print_at(0, 0, "################");
    print_at(0, 1, "0123456789ABCDEF");
    k_msleep(3000);

    for (unsigned int n = 0;; n++) {
        char line[LCD_COLS + 1];

        backlight = (n % 10 < 5) ? LCD_BACKLIGHT : 0; /* blinks every 5 s */
        snprintf(line, sizeof(line), "Uptime %-9u", n);
        print_at(0, 0, "LCD tester      ");
        print_at(0, 1, line);
        LOG_INF("%s backlight=%s", line, backlight ? "on" : "off");
        k_sleep(K_SECONDS(1));
    }
}
