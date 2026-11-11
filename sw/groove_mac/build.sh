#!/bin/sh
# Builds build/GrooveDrums.app: the Plinky groove sequencer and drum engine
# (sw/Core/Src/groove.c, drums.c) in a small macOS window. Needs only the
# Xcode command line tools (clang). Run: open build/GrooveDrums.app
set -e
cd "$(dirname "$0")"
APP=build/GrooveDrums.app
mkdir -p "$APP/Contents/MacOS"
clang -O2 -fobjc-arc -DEMU -I../Core/Src \
	-Wall -Wno-unused-function -Wno-unused-variable -Wno-unknown-attributes -Wno-ignored-attributes \
	main.m engine.c platform.c ../Core/Src/drums.c \
	-framework Cocoa -framework AudioToolbox \
	-o "$APP/Contents/MacOS/GrooveDrums"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>CFBundleName</key><string>Groove Drums</string>
	<key>CFBundleIdentifier</key><string>local.plinky.groovedrums</string>
	<key>CFBundleExecutable</key><string>GrooveDrums</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
</dict></plist>
PLIST
codesign --force --sign - "$APP" >/dev/null 2>&1 || true
echo "built $APP"
