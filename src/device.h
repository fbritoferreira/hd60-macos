#pragma once
#include <stdint.h>

enum {
	HD60_ERR_NOT_FOUND = -1,  // no HD60 plugged in
	HD60_ERR_DEVICE = -2,     // device error or unplugged mid-session; retrying makes sense
	HD60_ERR_FIRMWARE = -3,   // firmware file missing or corrupt; retrying won't help
};

// Called from the USB thread with a chunk of MPEG-TS (multiple of 188 bytes in practice).
typedef void (*hd60_data_cb)(const uint8_t *buf, int len, void *ctx);

// Open the HD60, boot it if needed, configure it and stream until *stop becomes non-zero
// or the device goes away. Returns 0 on a requested stop, or an HD60_ERR_* code.
int hd60_run(const char *firmware, hd60_data_cb cb, void *ctx, volatile int *stop, int verbose);
