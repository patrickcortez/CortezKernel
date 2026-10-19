/*
 * rtc.c — Runtime-Compiled CMOS Real-Time Clock (MC146818) Driver
 * Registers /dev/rtc0 character device and 'date' shell command at boot.
 */

#include <cortez.h>
#include <stdio.h>
#include <string.h>

static uint8_t cmos_read(uint8_t reg) {
    k_outb(0x70, reg);
    k_io_wait();
    return k_inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t val) {
    return (uint8_t)((val & 0x0F) + ((val >> 4) * 10));
}

static void read_rtc_datetime(int* year, int* month, int* day, int* hour, int* min, int* sec) {
    /* Wait while Update-In-Progress (UIP) bit is set */
    int timeout = 10000;
    while ((cmos_read(0x0A) & 0x80) != 0 && timeout > 0) {
        timeout--;
    }

    uint8_t s  = cmos_read(0x00);
    uint8_t m  = cmos_read(0x02);
    uint8_t h  = cmos_read(0x04);
    uint8_t d  = cmos_read(0x07);
    uint8_t mo = cmos_read(0x08);
    uint8_t y  = cmos_read(0x09);
    uint8_t status_b = cmos_read(0x0B);

    if ((status_b & 0x04) == 0) {
        s  = bcd_to_bin(s);
        m  = bcd_to_bin(m);
        h  = bcd_to_bin((uint8_t)(h & 0x7F)) | (uint8_t)(h & 0x80);
        d  = bcd_to_bin(d);
        mo = bcd_to_bin(mo);
        y  = bcd_to_bin(y);
    }

    if ((status_b & 0x02) == 0 && (h & 0x80) != 0) {
        h = (uint8_t)(((h & 0x7F) + 12) % 24);
    }

    *year  = 2000 + (int)y;
    *month = (int)mo;
    *day   = (int)d;
    *hour  = (int)h;
    *min   = (int)m;
    *sec   = (int)s;
}

static int64_t rtc_dev_read(void* ctx, void* buf, size_t count) {
    (void)ctx;
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    read_rtc_datetime(&year, &month, &day, &hour, &min, &sec);

    char tmp[64];
    int len = snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d %02d:%02d:%02d UTC\n",
                       year, month, day, hour, min, sec);
    if (len <= 0) {
        return 0;
    }
    size_t copy_len = (size_t)len;
    if (copy_len > count) {
        copy_len = count;
    }
    memcpy(buf, tmp, copy_len);
    return (int64_t)copy_len;
}

static bool rtc_dev_has_data(void* ctx) {
    (void)ctx;
    return true;
}

static int cmd_date(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    read_rtc_datetime(&year, &month, &day, &hour, &min, &sec);
    k_printf("CMOS RTC: %04d-%02d-%02d %02d:%02d:%02d UTC (uptime: %llu ms)\n",
             year, month, day, hour, min, sec, k_ticks());
    return 0;
}

static cortez_chardev_ops_t g_rtc_ops;

int module_init(void) {
    g_rtc_ops.read     = rtc_dev_read;
    g_rtc_ops.write    = NULL;
    g_rtc_ops.has_data = rtc_dev_has_data;

    if (k_register_chardev("rtc0", &g_rtc_ops, NULL) < 0) {
        k_log(KLOG_ERR, "rtc", "Failed to register /dev/rtc0");
        return -1;
    }

    if (k_register_command("date", "Display live CMOS Real-Time Clock date/time", "date", cmd_date) < 0) {
        k_unregister_chardev("rtc0");
        return -1;
    }

    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    read_rtc_datetime(&year, &month, &day, &hour, &min, &sec);
    k_log(KLOG_OK, "rtc", "CMOS RTC driver initialized (/dev/rtc0): %04d-%02d-%02d %02d:%02d:%02d UTC",
          year, month, day, hour, min, sec);
    return 0;
}

void module_exit(void) {
    k_unregister_command("date");
    k_unregister_chardev("rtc0");
    k_log(KLOG_INFO, "rtc", "CMOS RTC driver unloaded");
}
