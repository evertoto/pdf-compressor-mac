CC = clang
APP_NAME = PDF Compressor.app
APP_PATH = $(CURDIR)/$(APP_NAME)
BUNDLE_MACOS = $(APP_PATH)/Contents/MacOS
BUNDLE_FRAMEWORKS = $(APP_PATH)/Contents/Frameworks
BUNDLE_RESOURCES = $(APP_PATH)/Contents/Resources
BIN = pdfcompressor

CFLAGS = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null) -Wall -Wextra -headerpad_max_install_names
LIBS = $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

.PHONY: all clean app run help

help:
	@echo "Available targets:"
	@echo "  make        - Build the binary (pdfcompressor)"
	@echo "  make app    - Build the binary and package it into PDF Compressor.app"
	@echo "  make run    - Build the app (if needed) and launch PDF Compressor.app"
	@echo "  make clean  - Remove generated binary and app bundle"
	@echo "  make help   - Show this help message"

all: $(BIN)

$(BIN): main.c
	@echo "Compiling $(BIN)..."
	$(CC) $(CFLAGS) -o $(BIN) main.c $(LIBS)

app: $(BIN)
	@echo "Building $(APP_NAME)..."
	@rm -rf "$(APP_PATH)"
	@mkdir -p "$(BUNDLE_MACOS)" "$(BUNDLE_FRAMEWORKS)" "$(BUNDLE_RESOURCES)"
	@echo 'Copying executable...'
	@cp "$(BIN)" "$(BUNDLE_MACOS)/PDF Compressor"
	@chmod +x "$(BUNDLE_MACOS)/PDF Compressor"
	@echo 'Bundling dependencies...'
	@./bundle_deps.sh "$(BUNDLE_MACOS)/PDF Compressor" "$(BUNDLE_FRAMEWORKS)"
	@echo 'Writing Info.plist...'
	@printf '%s\n' \
		'<?xml version="1.0" encoding="UTF-8"?>' \
		'<!DOCTYPE plist PUBLIC "http://www.apple.com/DTDs/PropertyList-1.1" "http://www.apple.com/DTDs/plist-1.1.dtd">' \
		'<plist version="2">' \
		'  <dict>' \
		'    <key>CFBundleName</key>' \
		'    <string>PDF Compressor</string>' \
		'    <key>CFBundleDisplayName</key>' \
		'    <string>PDF Compressor</string>' \
		'    <key>CFBundleIdentifier</key>' \
		'    <string>com.everton.pdfcompressor</string>' \
		'    <key>CFBundleExecutable</key>' \
		'    <string>PDF Compressor</string>' \
		'    <key>CFBundlePackageType</key>' \
		'    <string>APPL</string>' \
		'    <key>CFBundleVersion</key>' \
		'    <string>1.0</string>' \
		'    <key>CFBundleType</key>' \
		'    <string>APPLICATION</string>' \
		'  </dict>' \
		'</plist>' \
		> "$(APP_PATH)/Contents/Info.plist"
	@echo 'Signing app...'
	@codesign --force --deep --sign - "$(APP_PATH)" 2>/dev/null || true
	@echo "Done: $(APP_PATH)"

run:
	@$(MAKE) app
	@open "$(APP_PATH)"

clean:
	@echo "Cleaning..."
	rm -f $(BIN)
	rm -rf "$(APP_PATH)"
