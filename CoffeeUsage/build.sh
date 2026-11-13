#!/bin/sh
set -eu
cd "$(dirname "$0")"

app=build/CoffeeUsage.app
sdk=$(xcrun --show-sdk-path)
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

for arch in arm64 x86_64; do
  for source in core bridge; do
    clang++ -std=c++20 -O2 -flto -fno-exceptions -fvisibility=hidden \
      -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=13.0 \
      -arch "$arch" -c "src/$source.cpp" -o "build/$source-$arch.o"
  done
  swiftc -O -swift-version 5 -parse-as-library -import-objc-header src/bridge.h \
    -target "$arch-apple-macosx13.0" -emit-object src/app.swift -o "build/app-$arch.o"
  # Link with clang++: swiftc adds compatibility libraries that pull in a
  # restricted SwiftUICore when targeting macOS 13.
  clang++ "build/core-$arch.o" "build/bridge-$arch.o" "build/app-$arch.o" \
    -arch "$arch" -mmacosx-version-min=13.0 -L "$sdk/usr/lib/swift" \
    -rpath /usr/lib/swift -framework Cocoa -framework Security \
    -framework ServiceManagement -o "build/CoffeeUsage-$arch"
done

lipo -create build/CoffeeUsage-arm64 build/CoffeeUsage-x86_64 \
  -output "$app/Contents/MacOS/CoffeeUsage"
strip -x "$app/Contents/MacOS/CoffeeUsage"
cp Info.plist "$app/Contents/Info.plist"
cp Assets/*.svg "$app/Contents/Resources/"
codesign --force --sign "${CODESIGN_ID:--}" --identifier local.usagebar "$app"
printf 'Built %s/%s\n' "$PWD" "$app"
