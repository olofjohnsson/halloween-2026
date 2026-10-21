#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(keypad_tester, LOG_LEVEL_INF);

/* Rows are driven low one at a time; columns have pull-ups. */
static const struct gpio_dt_spec rows[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(krow1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(krow2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(krow3), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(krow4), gpios),
};
static const struct gpio_dt_spec cols[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(kcol1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(kcol2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(kcol3), gpios),
};
static const char keymap[4][3] = {
    {'1', '2', '3'},
    {'4', '5', '6'},
    {'7', '8', '9'},
    {'*', '0', '#'},
};

#define KEYPAD_SETTLE_US 50
#define KEYPAD_RELEASE_MS 15
#define KEYPAD_POLL_MS 2

/* Returns the key currently pressed (scan only, never blocks). */
static char keypad_scan_pressed(void)
{
    for (size_t r = 0; r < ARRAY_SIZE(rows); r++) {
        for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
            (void)gpio_pin_set_dt(&rows[i], 1); /* inactive/high */
        }
        (void)gpio_pin_set_dt(&rows[r], 0); /* selected row low */
        k_busy_wait(KEYPAD_SETTLE_US);

        for (size_t c = 0; c < ARRAY_SIZE(cols); c++) {
            if (gpio_pin_get_dt(&cols[c]) == 0) {
                return keymap[r][c];
            }
        }
    }
    return '\0';
}

/* Reports a key the moment it is pressed (lowest latency) and ignores
 * contact bounce by requiring the key to be released for KEYPAD_RELEASE_MS. */
static char keypad_poll(void)
{
    static char held;
    static int64_t last_seen_ms;
    char key = keypad_scan_pressed();
    int64_t now = k_uptime_get();

    if (key) {
        last_seen_ms = now;
        if (key != held) {
            held = key;
            return key;
        }
    } else if (held && now - last_seen_ms >= KEYPAD_RELEASE_MS) {
        held = '\0';
    }
    return '\0';
}

int main(void)
{
    int rc;

    for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
        rc = gpio_pin_configure_dt(&rows[i], GPIO_OUTPUT_ACTIVE);
        if (rc < 0) {
            LOG_ERR("Row %u setup failed: %d", (unsigned int)(i + 1), rc);
            return 0;
        }
        (void)gpio_pin_set_dt(&rows[i], 1);
    }
    for (size_t i = 0; i < ARRAY_SIZE(cols); i++) {
        rc = gpio_pin_configure_dt(&cols[i], GPIO_INPUT | GPIO_PULL_UP);
        if (rc < 0) {
            LOG_ERR("Column %u setup failed: %d", (unsigned int)(i + 1), rc);
            return 0;
        }
    }

    LOG_INF("Keypad tester ready; press keys");

    while (1) {
        char key = keypad_poll();

        if (key) {
            LOG_INF("Key '%c'", key);
        }
        k_msleep(KEYPAD_POLL_MS);
    }

    return 0;
}
