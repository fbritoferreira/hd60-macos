// hd60: userspace driver for the Elgato Game Capture HD60 (0fd9:005c).
// Boots the card, replays the init sequence and writes the MPEG-TS stream to stdout.
//
//   hd60 [-f firmware/hd60-bridge.bin] | ffplay -fflags nobuffer -flags low_delay -
//   hd60 -u 127.0.0.1:5000      (OBS Media Source input: udp://127.0.0.1:5000)

#include "init_seq.h"

#include <arpa/inet.h>
#include <libusb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TIMEOUT 1000
#define NXFER 8
#define XFER_SIZE (188 * 1024)

static libusb_device_handle *h;
static volatile sig_atomic_t stop;
static int verbose;
static int udp = -1;
static struct sockaddr_in udp_to;

static void die(const char *what, int r) {
	fprintf(stderr, "hd60: %s: %s\n", what, r < 0 ? libusb_error_name(r) : "failed");
	exit(1);
}

static int ctrl_in(uint8_t req, uint16_t wv, uint16_t wi, uint8_t *buf, uint16_t len) {
	return libusb_control_transfer(h, 0xc0, req, wv, wi, buf, len, TIMEOUT);
}

static int ctrl_out(uint8_t req, uint16_t wv, uint16_t wi, const uint8_t *buf, uint16_t len) {
	return libusb_control_transfer(h, 0x40, req, wv, wi, (uint8_t *)buf, len, TIMEOUT);
}

static void wait_ready(void) {
	uint8_t b[2];
	for (int i = 0; i < 10000; i++) {
		int r = ctrl_in(REQ_STATUS, 0, 0, b, 2);
		if (r < 0) die("status", r);
		if (b[1] & 1) return;
	}
	die("I2C engine never became ready", 0);
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
static void load_firmware(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		fprintf(stderr, "hd60: extract it first: tools/extract-firmware.py\n");
		exit(1);
	}
	uint8_t rec[32];
	int n, count = 0;
	while (fread(rec, 1, 1, f) == 1) {
		int len = 5 + rec[0];
		if (rec[0] > 16 || fread(rec + 1, 1, len - 1, f) != (size_t)len - 1) die("corrupt firmware file", 0);
		int r = libusb_bulk_transfer(h, EP_FW_OUT, rec, len, &n, TIMEOUT);
		if (r || n != len) die("firmware write", r);
		count++;
	}
	fclose(f);
	// Start execution at the load base.
	static const uint8_t go[] = {0x08, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00};
	int r = libusb_bulk_transfer(h, EP_FW_OUT, (uint8_t *)go, sizeof go, &n, TIMEOUT);
	if (r) die("firmware start", r);
	for (int i = 0; i < 100 && !booted(); i++) usleep(10000);
	if (!booted()) die("bridge did not come up after firmware load", 0);
	fprintf(stderr, "hd60: loaded %d firmware records\n", count);
}

static void run_init(void) {
	uint8_t buf[64];
	size_t n = sizeof hd60_init_seq / sizeof hd60_init_seq[0];
	for (size_t i = 0; i < n && !stop; i++) {
		const struct hd60_op *op = &hd60_init_seq[i];
		int r;
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
				if (tries > 5000) {
					fprintf(stderr, "hd60: poll timeout at op %zu (got %02x)\n", i, buf[0]);
					exit(1);
				}
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
		if (verbose && i % 200 == 0) fprintf(stderr, "hd60: init %zu/%zu\n", i, n);
	}
}

static void LIBUSB_CALL on_data(struct libusb_transfer *t) {
	if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length) {
		if (udp >= 0) {
			// 7 TS packets per datagram, the usual MPEG-TS-over-UDP size. Drops are tolerated.
			for (int off = 0; off < t->actual_length; off += 1316) {
				int n = t->actual_length - off < 1316 ? t->actual_length - off : 1316;
				sendto(udp, t->buffer + off, n, 0, (struct sockaddr *)&udp_to, sizeof udp_to);
			}
		} else {
			if (fwrite(t->buffer, 1, t->actual_length, stdout) != (size_t)t->actual_length) stop = 1;
			fflush(stdout);
		}
	} else if (t->status != LIBUSB_TRANSFER_COMPLETED && t->status != LIBUSB_TRANSFER_TIMED_OUT) {
		fprintf(stderr, "hd60: transfer status %d\n", t->status);
		stop = 1;
	}
	if (!stop && libusb_submit_transfer(t) < 0) stop = 1;
}

static void stream(void) {
	struct libusb_transfer *x[NXFER];
	for (int i = 0; i < NXFER; i++) {
		x[i] = libusb_alloc_transfer(0);
		libusb_fill_bulk_transfer(x[i], h, EP_TS_IN, malloc(XFER_SIZE), XFER_SIZE, on_data, NULL, 0);
		int r = libusb_submit_transfer(x[i]);
		if (r) die("submit", r);
	}
	fprintf(stderr, "hd60: streaming\n");
	while (!stop) libusb_handle_events(NULL);
	for (int i = 0; i < NXFER; i++) libusb_cancel_transfer(x[i]);
}

static void on_signal(int s) {
	(void)s;
	stop = 1;
}

int main(int argc, char **argv) {
	const char *fw = "firmware/hd60-bridge.bin";
	int c;
	while ((c = getopt(argc, argv, "f:u:v")) != -1) {
		if (c == 'f') fw = optarg;
		else if (c == 'v') verbose = 1;
		else if (c == 'u') {
			char host[64];
			int port;
			if (sscanf(optarg, "%63[^:]:%d", host, &port) != 2 || inet_pton(AF_INET, host, &udp_to.sin_addr) != 1) {
				fprintf(stderr, "hd60: -u wants ip:port\n");
				return 2;
			}
			udp_to.sin_family = AF_INET;
			udp_to.sin_port = htons(port);
			udp = socket(AF_INET, SOCK_DGRAM, 0);
		} else {
			fprintf(stderr, "usage: hd60 [-v] [-f firmware.bin] [-u ip:port | > out.ts]\n");
			return 2;
		}
	}
	if (udp < 0 && isatty(1)) {
		fprintf(stderr, "hd60: refusing to write video to a terminal; pipe it somewhere\n");
		return 2;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, on_signal);

	int r = libusb_init(NULL);
	if (r) die("libusb_init", r);
	h = libusb_open_device_with_vid_pid(NULL, HD60_VID, HD60_PID);
	if (!h) die("HD60 not found (or in use by another app)", 0);
	if ((r = libusb_claim_interface(h, 0))) die("claim interface", r);

	if (!booted()) load_firmware(fw);
	else fprintf(stderr, "hd60: bridge firmware already running\n");
	run_init();
	if (!stop) stream();

	libusb_release_interface(h, 0);
	libusb_close(h);
	libusb_exit(NULL);
	return 0;
}
