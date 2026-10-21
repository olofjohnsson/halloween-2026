#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(blink_led), gpios);

int main(void)
{
    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) {
        return 0;
    }

    while (1) {
        (void)gpio_pin_toggle_dt(&led);
        k_msleep(500);
    }

    return 0;
}
