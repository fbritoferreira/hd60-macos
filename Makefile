CFLAGS += -O2 -Wall -Wextra -Wno-missing-field-initializers $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)

hd60: src/hd60.c src/device.c src/device.h src/hd60.h src/init_seq.h
	$(CC) $(CFLAGS) -o $@ src/hd60.c src/device.c $(LDLIBS)

clean:
	rm -f hd60
