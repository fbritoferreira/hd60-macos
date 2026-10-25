#pragma once
#include <stdint.h>

#define HD60_VID 0x0fd9
#define HD60_PID 0x005c

#define EP_FW_OUT 0x01   // firmware upload (bulk)
#define EP_TS_IN 0x82    // MPEG-TS stream (bulk)

// Vendor requests on EP0.
#define REQ_ID 0x02      // IN 4 bytes: VID/PID; stalls until bridge firmware runs
#define REQ_STATUS 0x06  // IN 2 bytes: 00 01 = I2C engine ready
#define REQ_I2C_R 0x12   // IN: wValue = (0xC0|len)<<8 | addr, wIndex = 0x0010
#define REQ_I2C_W 0x13   // OUT: 13, addr, 0xC0|len, 0x10, payload[len]

enum { OP_I2C_W, OP_I2C_R, OP_I2C_POLL, OP_CTRL_OUT, OP_CTRL_IN };

struct hd60_op {
	uint8_t kind;
	uint8_t a;        // I2C address (8-bit form) or request number
	uint8_t len;      // payload / read length
	uint8_t data[20];
	uint8_t mask;     // POLL: bits to compare
	uint16_t wValue;  // CTRL_*
	uint16_t wIndex;  // CTRL_*
	uint8_t expect;   // POLL: wanted value of (read & mask)
};

struct hd60_mode {
	const char *name;
	const struct hd60_op *seq;
	unsigned long len;
};
