#include <stdio.h>

#include "arc.h"
#include "pico/stdlib.h"
#include "spdif.h"
#include "tusb.h"
#include "usb_audio.h"

#define SPDIF_PIN 2
#define CEC_PIN 3
#define HPD_PIN 4

static void wait_for_console_attach(void) {
    absolute_time_t deadline = make_timeout_time_ms(5000);

    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        tud_task();
        sleep_ms(1);
    }
}

int main(void) {
    tusb_init();
    stdio_init_all();

    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);

    wait_for_console_attach();

    printf("\nPicoARC bring-up\n");

    spdif_start(SPDIF_PIN);
    spdif_set_mode(SPDIF_MODE_SILENCE);
    arc_init(CEC_PIN, HPD_PIN);

    printf("spdif: GP%d 48k stereo %s\n", SPDIF_PIN, spdif_mode_name(spdif_get_mode()));
    printf("arc: ready\n");

    absolute_time_t next_blink = make_timeout_time_ms(250);
    bool led_on = false;

    while (true) {
        tud_task();
        usb_audio_task();
        arc_task();

        if (absolute_time_diff_us(get_absolute_time(), next_blink) <= 0) {
            led_on = !led_on;
            gpio_put(led_pin, led_on);
            next_blink = make_timeout_time_ms(250);
        }

        tight_loop_contents();
    }
}
