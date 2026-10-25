// HD60 device core: boot, init and stream one session. Shared by the CLI and the camera extension.

#include "device.h"
#include "init_seq.h"

#include <libusb.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TIMEOUT 1000
#define NXFER 8
#define XFER_SIZE (188 * 1024)

// ponytail: one device per process (global handle + jmp_buf); fine for a single capture card.
static libusb_device_handle *h;
static jmp_buf fail;
static int fail_usb;      // libusb error code, or 0
static int fail_firmware; // failure is about the firmware file, not the device
static const char *fail_what;
static long cur_op = -1; // init step in progress, for error reports

// Abort the session. usb_err is a libusb error code, or 0 when there isn't one.
static void die(const char *what, int usb_err) {
	fail_what = what;
	fail_usb = usb_err < 0 ? usb_err : 0;
	longjmp(fail, 1);
}

static void die_firmware(const char *what) {
	fail_firmware = 1;
	die(what, 0);
}

static int ctrl_in(uint8_t req, uint16_t wv, uint16_t wi, uint8_t *buf, uint16_t len) {
	return libusb_control_transfer(h, 0xc0, req, wv, wi, buf, len, TIMEOUT);
}

static int ctrl_out(uint8_t req, uint16_t wv, uint16_t wi, const uint8_t *buf, uint16_t len) {
	return libusb_control_transfer(h, 0x40, req, wv, wi, (uint8_t *)buf, len, TIMEOUT);
}

// Status 00 01 = I2C engine idle. Game Capture HD waits at most ~100 ms and then proceeds anyway
// (during shutdown the flag never comes back), so do the same; only many timeouts in a row
// mean the card is wedged.
static int ready_timeouts;

static void wait_ready(void) {
	uint8_t b[2];
	for (int i = 0; i < 2500; i++) { // ~250 ms at ~0.1 ms per request
		int r = ctrl_in(REQ_STATUS, 0, 0, b, 2);
		if (r < 0) die("status", r);
		if (b[1] & 1) {
			ready_timeouts = 0;
			return;
		}
	}
	if (++ready_timeouts > 20) die("I2C engine stuck busy", 0);
}

static void i2c_write(uint8_t addr, const uint8_t *p, uint8_t n) {
	uint8_t buf[24] = {REQ_I2C_W, addr, 0xc0 | n, 0x10};
	memcpy(buf + 4, p, n);
	wait_ready();
	int r = ctrl_out(REQ_I2C_W, 0, 0, buf, 4 + n);
	if (r != 4 + n) die("i2c write", r);
}

static void i2c_read(uint8_t addr, uint8_t *p, uint8_t n) {
	wait_ready();
	int r = ctrl_in(REQ_I2C_R, (0xc0 | n) << 8 | addr, 0x0010, p, n);
	if (r != n) die("i2c read", r);
}

static int booted(void) {
	uint8_t id[4];
	return ctrl_in(REQ_ID, 0x01a1, 0, id, 4) == 4;
}

// Firmware file is a stream of records: len, addr(2), type, data[len], checksum.
static void load_firmware(const char *path, int verbose) {
	FILE *f = fopen(path, "rb");
	if (!f) die_firmware("cannot open firmware (run tools/extract-firmware.py)");
	uint8_t rec[32];
	int n, count = 0;
	while (fread(rec, 1, 1, f) == 1) {
		int len = 5 + rec[0];
		if (rec[0] > 16 || fread(rec + 1, 1, len - 1, f) != (size_t)len - 1) {
			fclose(f);
			die_firmware("corrupt firmware file");
		}
		int r = libusb_bulk_transfer(h, EP_FW_OUT, rec, len, &n, TIMEOUT);
		if (r || n != len) {
			fclose(f);
			die("firmware write", r);
		}
		count++;
	}
	fclose(f);
	// Start execution at the load base.
	static const uint8_t go[] = {0x08, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00};
	int r = libusb_bulk_transfer(h, EP_FW_OUT, (uint8_t *)go, sizeof go, &n, TIMEOUT);
	if (r) die("firmware start", r);
	for (int i = 0; i < 100 && !booted(); i++) usleep(10000);
	if (!booted()) die("bridge did not come up after firmware load", 0);
	if (verbose) fprintf(stderr, "hd60: loaded %d firmware records\n", count);
}

static void run_seq(const struct hd60_op *seq, size_t n, volatile int *stop, int verbose) {
	uint8_t buf[64];
	for (size_t i = 0; i < n && !(stop && *stop); i++) {
		const struct hd60_op *op = &seq[i];
		int r;
		cur_op = (long)i;
		switch (op->kind) {
		case OP_I2C_W:
			i2c_write(op->a, op->data, op->len);
			break;
		case OP_I2C_R:
			i2c_read(op->a, buf, op->len);
			break;
		case OP_I2C_POLL:
			for (int tries = 0;; tries++) {
				i2c_write(op->a, op->data, op->len);
				i2c_read(op->a, buf, 1);
				if ((buf[0] & op->mask) == op->expect) break;
				if (tries > 5000) die("register poll timed out", 0);
			}
			break;
		case OP_CTRL_OUT:
			r = ctrl_out(op->a, op->wValue, op->wIndex, op->data, op->len);
			if (r != op->len) die("control out", r);
			break;
		case OP_CTRL_IN:
			r = ctrl_in(op->a, op->wValue, op->wIndex, buf, op->len);
			if (r < 0) die("control in", r);
			break;
		}
		if (verbose && i % 500 == 0) fprintf(stderr, "hd60: step %zu/%zu\n", i, n);
	}
}

// Clean shutdown, as Game Capture HD does on quit: stop the encoder, power down the HDMI
// receiver, then the processor handshake from elgato-gchd (ab a9 0f a4 55 reads 33 44 55 once
// the processor is off). Skipping this can leave the card needing a replug.
static void shutdown_card(int verbose) {
	cur_op = -1;
	run_seq(hd60_stop_seq, sizeof hd60_stop_seq / sizeof hd60_stop_seq[0], NULL, 0);
	static const uint8_t probe[] = {0xab, 0xa9, 0x0f, 0xa4, 0x55};
	static const uint8_t off[] = {0x0c, 0x0b, 0x80};
	uint8_t r[3];
	for (int i = 0; i < 50; i++) {
		i2c_write(0x66, probe, sizeof probe);
		i2c_read(0x66, r, 3);
		ctrl_out(0x0c, 0, 0, off, sizeof off);
		if (r[0] == 0x33 && r[1] == 0x44 && r[2] == 0x55) {
			if (verbose) fprintf(stderr, "hd60: card shut down\n");
			return;
		}
		usleep(100000);
	}
	if (verbose) fprintf(stderr, "hd60: shutdown handshake did not complete\n");
}

struct stream_state {
	hd60_data_cb cb;
	void *ctx;
	int inflight;
	int lost;
	volatile int *stop;
};

static void LIBUSB_CALL on_data(struct libusb_transfer *t) {
	struct stream_state *s = t->user_data;
	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		if (t->actual_length) s->cb(t->buffer, t->actual_length, s->ctx);
	} else if (t->status != LIBUSB_TRANSFER_TIMED_OUT && t->status != LIBUSB_TRANSFER_CANCELLED) {
		s->lost = 1;
	}
	if (*s->stop || s->lost || libusb_submit_transfer(t) < 0) {
		s->lost |= !*s->stop && t->status != LIBUSB_TRANSFER_CANCELLED;
		s->inflight--;
	}
}

static void stream(struct stream_state *s) {
	struct libusb_transfer *x[NXFER] = {0};
	for (int i = 0; i < NXFER; i++) {
		x[i] = libusb_alloc_transfer(0);
		libusb_fill_bulk_transfer(x[i], h, EP_TS_IN, malloc(XFER_SIZE), XFER_SIZE, on_data, s, 0);
		if (libusb_submit_transfer(x[i]) == 0) s->inflight++;
		else s->lost = 1;
	}
	while (!*s->stop && !s->lost) libusb_handle_events(NULL);
	for (int i = 0; i < NXFER; i++) libusb_cancel_transfer(x[i]);
	// Transfers must finish cancelling before their buffers are freed.
	struct timeval tv = {0, 100000};
	for (int i = 0; s->inflight > 0 && i < 50; i++) libusb_handle_events_timeout(NULL, &tv);
	for (int i = 0; i < NXFER; i++) {
		free(x[i]->buffer);
		libusb_free_transfer(x[i]);
	}
}

int hd60_run(const char *firmware, hd60_data_cb cb, void *ctx, volatile int *stop, int verbose) {
	static int inited;
	if (!inited) {
		int r = libusb_init(NULL);
		if (r) return HD60_ERR_DEVICE;
		inited = 1;
	}
	h = libusb_open_device_with_vid_pid(NULL, HD60_VID, HD60_PID);
	if (!h) return HD60_ERR_NOT_FOUND;
	int ret = 0;
	fail_firmware = 0;
	ready_timeouts = 0;
	cur_op = -1;
	if (setjmp(fail)) {
		if (verbose)
			fprintf(stderr, "hd60: %s%s%s (init step %ld)\n", fail_what, fail_usb ? ": " : "",
				fail_usb ? libusb_error_name(fail_usb) : "", cur_op);
		ret = fail_firmware ? HD60_ERR_FIRMWARE : HD60_ERR_DEVICE;
		goto out;
	}
	int r = libusb_claim_interface(h, 0);
	if (r) die("claim interface (is another app using the HD60?)", r);
	if (!booted()) load_firmware(firmware, verbose);
	run_seq(hd60_init_seq, sizeof hd60_init_seq / sizeof hd60_init_seq[0], stop, verbose);
	cur_op = -1;
	if (!*stop) {
		struct stream_state s = {cb, ctx, 0, 0, stop};
		if (verbose) fprintf(stderr, "hd60: streaming\n");
		stream(&s);
		if (s.lost) ret = HD60_ERR_DEVICE;
	}
	if (ret == 0) shutdown_card(verbose);
	libusb_release_interface(h, 0);
out:
	libusb_close(h);
	h = NULL;
	return ret;
}
