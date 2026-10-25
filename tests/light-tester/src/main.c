/*
 * Light effect tester: plays scary, celebration and wrong-answer patterns on a PWM output
 * (P0.13 -> MOSFET gate -> spot light). Button 1 skips to the next pattern.
 * Patterns are tables of steps (ramp to an intensity, then hold) so they are easy to port.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/random/random.h>
#include <stdio.h>

static const struct pwm_dt_spec light = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));
static const struct gpio_dt_spec skip_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

struct step {
    uint8_t pct;      /* target intensity 0..100 */
    uint16_t ramp_ms; /* linear ramp from the current intensity (0 = jump) */
    uint16_t hold_ms; /* hold time after reaching the target */
};

struct pattern {
    const char *name;
    const struct step *steps;
    size_t count;
    unsigned int repeat;
    bool (*run)(void); /* used instead of the table for random patterns */
};

static int current_pct;

/* Squared curve: eye perceives intensity roughly logarithmically, so ramps look even. */
static void set_light(int pct)
{
    uint32_t pulse = (uint32_t)((uint64_t)light.period * pct * pct / 10000U);

    current_pct = pct;
    (void)pwm_set_pulse_dt(&light, pulse);
}

/* Waits ms; returns true if the skip button was pressed. */
static bool wait_ms(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        if (gpio_pin_get_dt(&skip_button) > 0) {
            while (gpio_pin_get_dt(&skip_button) > 0) {
                k_msleep(10);
            }
            return true;
        }
        k_msleep(MIN(5, ms - t));
    }
    return false;
}

static bool play_step(uint8_t pct, uint16_t ramp_ms, uint16_t hold_ms)
{
    int from = current_pct;

    if (ramp_ms == 0) {
        set_light(pct);
    } else {
        for (int t = 10; t <= ramp_ms; t += 10) {
            set_light(from + ((int)pct - from) * t / ramp_ms);
            if (wait_ms(10)) return true;
        }
        set_light(pct);
    }
    return hold_ms && wait_ms(hold_ms);
}

static uint32_t rnd(uint32_t min, uint32_t max)
{
    return min + sys_rand32_get() % (max - min + 1);
}

#define S(pct, ramp, hold) { pct, ramp, hold }

/* ---------- Scary ---------- */

/* Two thumps, a pause, repeat: a heart beating in the dark. */
static const struct step heartbeat[] = {
    S(100, 50, 0), S(15, 120, 0), S(70, 50, 0), S(0, 250, 700),
};

/* Dark, a stuttering lightning strike, long darkness. */
static const struct step lightning[] = {
    S(0, 0, 1200), S(100, 10, 40), S(10, 30, 60), S(100, 10, 30), S(5, 40, 0),
    S(80, 10, 80), S(0, 300, 1500),
};

/* Slow dread: swells up and fades away. */
static const struct step breathing[] = {
    S(55, 2500, 300), S(0, 3000, 600),
};

/* A dying bulb: random levels and sudden blackouts. */
static bool dying_bulb(void)
{
    for (int i = 0; i < 40; i++) {
        bool blackout = rnd(0, 7) == 0;

        if (play_step(blackout ? 0 : rnd(5, 100), 0, blackout ? rnd(300, 900) : rnd(20, 120))) {
            return true;
        }
    }
    return false;
}

/* ---------- Celebration ---------- */

static const struct step strobe[] = {
    S(100, 0, 60), S(0, 0, 60),
};

/* Pulses get faster and faster, then a bright finish. */
static const struct step build_up[] = {
    S(100, 300, 0), S(0, 300, 0), S(100, 200, 0), S(0, 200, 0), S(100, 100, 0), S(0, 100, 0),
    S(100, 60, 0), S(0, 60, 0), S(100, 30, 0), S(0, 30, 0), S(100, 0, 1500), S(0, 600, 400),
};

/* Fireworks: random bright bursts that fade out, sometimes doubled. */
static bool fireworks(void)
{
    for (int i = 0; i < 8; i++) {
        if (play_step(100, 0, 40)) return true;
        if (rnd(0, 2) == 0 && (play_step(0, 0, 70) || play_step(100, 0, 40))) return true;
        if (play_step(0, rnd(300, 700), rnd(100, 400))) return true;
    }
    return false;
}

/* ---------- Wrong answer ---------- */

/* Three hard buzzer flashes and a slow fade. */
static const struct step buzzer[] = {
    S(100, 0, 300), S(0, 0, 150), S(100, 0, 300), S(0, 0, 150), S(100, 0, 300), S(0, 0, 150),
    S(25, 0, 200), S(0, 800, 0),
};

/* Fizzles out: stuttering flashes that get weaker. */
static const struct step fizzle[] = {
    S(100, 0, 100), S(0, 0, 60), S(80, 0, 80), S(0, 0, 100), S(60, 0, 60), S(0, 0, 160),
    S(40, 0, 40), S(0, 0, 300), S(25, 0, 0), S(0, 1000, 0),
};

#define TABLE(n) n, ARRAY_SIZE(n)

static const struct pattern patterns[] = {
    {"SCARY: heartbeat", TABLE(heartbeat), 6, NULL},
    {"SCARY: lightning", TABLE(lightning), 3, NULL},
    {"SCARY: slow breathing", TABLE(breathing), 3, NULL},
    {"SCARY: dying bulb", NULL, 0, 1, dying_bulb},
    {"CELEBRATION: strobe", TABLE(strobe), 20, NULL},
    {"CELEBRATION: build-up", TABLE(build_up), 1, NULL},
    {"CELEBRATION: fireworks", NULL, 0, 1, fireworks},
    {"WRONG: buzzer", TABLE(buzzer), 1, NULL},
    {"WRONG: fizzle", TABLE(fizzle), 1, NULL},
};

static void play(const struct pattern *p)
{
    printk("Playing %s (button 1 = skip)\n", p->name);

    if (p->run) {
        (void)p->run();
        return;
    }
    for (unsigned int r = 0; r < p->repeat; r++) {
        for (size_t i = 0; i < p->count; i++) {
            if (play_step(p->steps[i].pct, p->steps[i].ramp_ms, p->steps[i].hold_ms)) {
                return;
            }
        }
    }
}

int main(void)
{
    if (!pwm_is_ready_dt(&light) || !gpio_is_ready_dt(&skip_button) ||
        gpio_pin_configure_dt(&skip_button, GPIO_INPUT) < 0) {
        printk("PWM or button not ready\n");
        return 0;
    }

    while (1) {
        for (size_t i = 0; i < ARRAY_SIZE(patterns); i++) {
            play(&patterns[i]);
            set_light(0);
            k_msleep(1500);
        }
    }

    return 0;
}
