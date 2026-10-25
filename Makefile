CFLAGS += -O2 -Wall -Wextra -Wno-missing-field-initializers $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)

hd60: src/hd60.c src/device.c src/device.h src/hd60.h src/init_seq.h
	$(CC) $(CFLAGS) -o $@ src/hd60.c src/device.c $(LDLIBS)

clean:
	rm -f hd60

# ---- OBS plugin (links OBS's own libobs and FFmpeg; headers from obs-plugin/deps, see obs-plugin/README.md)
OBS_APP ?= /Applications/OBS.app
OBS_FW = $(OBS_APP)/Contents/Frameworks
PLUGIN = obs-plugin/build/hd60.plugin
PLUGIN_DIR = $(HOME)/Library/Application Support/obs-studio/plugins

obs-plugin: $(PLUGIN)/Contents/MacOS/hd60

$(PLUGIN)/Contents/MacOS/hd60: obs-plugin/hd60-source.c src/device.c src/device.h src/init_seq.h obs-plugin/Info.plist
	mkdir -p $(PLUGIN)/Contents/MacOS $(PLUGIN)/Contents/Resources
	cp obs-plugin/Info.plist $(PLUGIN)/Contents/
	$(CC) -O2 -Wall -Wno-missing-field-initializers -bundle -arch arm64 -mmacosx-version-min=13.0 \
		-Isrc -Iobs-plugin/include -Iobs-plugin/deps/obs-studio/libobs -Iobs-plugin/deps/ffmpeg -I/opt/homebrew/include \
		$(shell pkg-config --cflags libusb-1.0) \
		-o $@ obs-plugin/hd60-source.c src/device.c \
		-F$(OBS_FW) -framework libobs -L$(OBS_FW) -lavformat -lavcodec -lavutil \
		/opt/homebrew/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security
	codesign -s - -f $(PLUGIN)

install-obs-plugin: obs-plugin
	test -f firmware/hd60-bridge.bin || { echo "run tools/extract-firmware.py first"; exit 1; }
	cp firmware/hd60-bridge.bin $(PLUGIN)/Contents/Resources/
	codesign -s - -f $(PLUGIN)
	mkdir -p "$(PLUGIN_DIR)"
	rm -rf "$(PLUGIN_DIR)/hd60.plugin"
	cp -R $(PLUGIN) "$(PLUGIN_DIR)/"

.PHONY: obs-plugin install-obs-plugin clean
