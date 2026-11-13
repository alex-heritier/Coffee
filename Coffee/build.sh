#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build/Coffee.app/Contents/MacOS
mkdir -p build/Coffee.app/Contents/Resources
swiftc -O -parse-as-library -target "$(uname -m)-apple-macosx13.0" Coffee.swift -o build/Coffee.app/Contents/MacOS/Coffee
cp Info.plist build/Coffee.app/Contents/Info.plist
cp Assets/Coffee.icns build/Coffee.app/Contents/Resources/Coffee.icns
codesign --force --sign - build/Coffee.app
printf 'Built %s/build/Coffee.app\n' "$PWD"
