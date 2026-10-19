/*
 * beep.c — PC Speaker Tone Generator (PIT Channel 2, Ports 0x42/0x43/0x61)
 * Usage: beep [freq_hz] [duration_ms]
 */

#include <cortez.h>
#include <stdlib.h>

static void speaker_on(uint32_t freq_hz) {
    if (freq_hz < 20)    freq_hz = 20;
    if (freq_hz > 20000) freq_hz = 20000;

    uint32_t divisor = 1193180U / freq_hz;
    k_outb(0x43, 0xB6);
    k_outb(0x42, (uint8_t)(divisor & 0xFF));
    k_outb(0x42, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t gate = k_inb(0x61);
    if ((gate & 0x03) != 0x03) {
        k_outb(0x61, (uint8_t)(gate | 0x03));
    }
}

static void speaker_off(void) {
    uint8_t gate = k_inb(0x61);
    k_outb(0x61, (uint8_t)(gate & 0xFC));
}

static int cmd_beep(int argc, const char** argv) {
    uint32_t freq = 440;
    uint32_t dur  = 150;
    if (argc > 1) {
        int f = atoi(argv[1]);
        if (f > 0) freq = (uint32_t)f;
    }
    if (argc > 2) {
        int d = atoi(argv[2]);
        if (d > 0) dur = (uint32_t)d;
    }

    k_printf("Playing PC speaker tone: %u Hz for %u ms...\n", freq, dur);
    speaker_on(freq);
    k_sleep_ms(dur);
    speaker_off();
    return 0;
}

int module_init(void) {
    return k_register_command("beep",
                              "Play a tone on the PC speaker via PIT Channel 2",
                              "beep [freq_hz] [duration_ms]",
                              cmd_beep);
}

void module_exit(void) {
    speaker_off();
    k_unregister_command("beep");
}
