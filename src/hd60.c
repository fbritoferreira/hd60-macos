// hd60: userspace driver for the Elgato Game Capture HD60 (0fd9:005c).
// Boots the card, configures it and writes the MPEG-TS stream to stdout or UDP.
// Keeps running across unplug/replug until interrupted.
//
//   hd60 [-f firmware/hd60-bridge.bin] | ffplay -fflags nobuffer -flags low_delay -
//   hd60 -u 127.0.0.1:5000      (OBS Media Source input: udp://127.0.0.1:5000)

#include "device.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int stop;
static int udp = -1;
static struct sockaddr_in udp_to;

static void on_data(const uint8_t *buf, int len, void *ctx) {
	(void)ctx;
	if (udp >= 0) {
		// 7 TS packets per datagram, the usual MPEG-TS-over-UDP size. Drops are tolerated.
		for (int off = 0; off < len; off += 1316) {
			int n = len - off < 1316 ? len - off : 1316;
			sendto(udp, buf + off, n, 0, (struct sockaddr *)&udp_to, sizeof udp_to);
		}
	} else {
		if (fwrite(buf, 1, len, stdout) != (size_t)len) stop = 1;
		fflush(stdout);
	}
}

static void on_signal(int s) {
	(void)s;
	stop = 1;
}

int main(int argc, char **argv) {
	struct hd60_config cfg = {.firmware = "firmware/hd60-bridge.bin"};
	int c;
	while ((c = getopt(argc, argv, "f:m:b:u:v")) != -1) {
		if (c == 'f') cfg.firmware = optarg;
		else if (c == 'm') cfg.mode = optarg;
		else if (c == 'b') cfg.bitrate_kbps = atoi(optarg);
		else if (c == 'v') cfg.verbose = 1;
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
			fprintf(stderr, "usage: hd60 [-v] [-f firmware.bin] [-m mode] [-b kbps] [-u ip:port | > out.ts]\n");
			fprintf(stderr, "modes:");
			for (const char *const *m = hd60_mode_names(); *m; m++) fprintf(stderr, " %s", *m);
			fprintf(stderr, "\nbitrate: average kbps, up to 40000 (default: Elgato's, ~40000)\n");
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

	int last = 1;
	while (!stop) {
		int r = hd60_run(&cfg, on_data, NULL, &stop);
		if (r == HD60_ERR_FIRMWARE) {
			fprintf(stderr, "hd60: firmware %s missing or corrupt; run tools/extract-firmware.py\n", cfg.firmware);
			return 1;
		}
		if (r == HD60_ERR_CONFIG) {
			fprintf(stderr, "hd60: unknown mode or bitrate out of range (run hd60 -h)\n");
			return 2;
		}
		if (stop) break;
		if (r != last) {
			if (r == HD60_ERR_NOT_FOUND) fprintf(stderr, "hd60: waiting for the HD60 to be plugged in\n");
			else if (r == HD60_ERR_DEVICE) fprintf(stderr, "hd60: lost the HD60, reconnecting\n");
			else fprintf(stderr, "hd60: stream ended, restarting\n");
			last = r;
		}
		sleep(1);
	}
	return 0;
}
