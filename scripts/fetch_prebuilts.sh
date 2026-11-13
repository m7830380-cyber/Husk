#!/bin/bash
# Fetch prebuilt heavy dependencies from Husk 3.4 release and MoltenVK v1.4.2
# (QEMU dylib, ANGLE, MoltenVK, libhuskps4, guests, android-system, java-runtime)
# so full rebuilds of the app, translation layer, and java runtime can be performed
# quickly without spending hours cross-compiling QEMU or requiring a local Lineage OS disk.
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> Downloading prebuilts from Husk 3.4 release"
IPA_URL="https://github.com/Leviidev/Husk/releases/download/3.4/Husk.ipa"
curl -fL --retry 3 -o "$WORK/Husk.ipa" "$IPA_URL"

echo "==> Unpacking Husk.ipa"
unzip -q "$WORK/Husk.ipa" -d "$WORK/unpacked"
APP="$WORK/unpacked/Payload/Husk.app"

# 1. Frameworks & dylibs
echo "==> Staging iOS libraries"
mkdir -p "$HUSK_ROOT/build/ios-arm64/lib"
mkdir -p "$HUSK_ROOT/build/ios-arm64/sysroot/lib"

cp "$APP/Frameworks/libqemu-aarch64-softmmu.dylib" "$HUSK_ROOT/build/ios-arm64/lib/"
cp "$APP/Frameworks/libANGLE-shared.dylib" "$HUSK_ROOT/build/ios-arm64/sysroot/lib/"
cp "$APP/Frameworks/libhuskps4.dylib" "$HUSK_ROOT/build/ios-arm64/lib/"

# MoltenVK.xcframework from official KhronosGroup release
echo "==> Staging MoltenVK.xcframework"
MVK_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar"
if curl -fL --retry 3 -o "$WORK/MoltenVK-ios.tar" "$MVK_URL"; then
    tar -xf "$WORK/MoltenVK-ios.tar" -C "$WORK"
    rm -rf "$HUSK_ROOT/build/ios-arm64/lib/MoltenVK.xcframework"
    cp -R "$WORK/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework" "$HUSK_ROOT/build/ios-arm64/lib/"
elif command -v xcodebuild >/dev/null 2>&1 && [ -d "$APP/Frameworks/MoltenVK.framework" ]; then
    rm -rf "$HUSK_ROOT/build/ios-arm64/lib/MoltenVK.xcframework"
    xcodebuild -create-xcframework -framework "$APP/Frameworks/MoltenVK.framework" -output "$HUSK_ROOT/build/ios-arm64/lib/MoltenVK.xcframework"
fi

# 2. Java runtime base (jars, so's, etc.)
echo "==> Staging Java runtime base"
mkdir -p "$HUSK_ROOT/build/java-runtime"
cp -R "$APP/java-runtime/"* "$HUSK_ROOT/build/java-runtime/"

# 3. Android system (fonts, cacerts)
echo "==> Staging Android system"
mkdir -p "$HUSK_ROOT/build/android-system"
cp -R "$APP/android-system/"* "$HUSK_ROOT/build/android-system/"

# 4. Phase 0 guest (vmlinuz-virt, initramfs-virt)
echo "==> Staging guest images"
mkdir -p "$HUSK_ROOT/guests/phase0"
cp "$APP/vmlinuz-virt" "$HUSK_ROOT/guests/phase0/"
cp "$APP/initramfs-virt" "$HUSK_ROOT/guests/phase0/"

# 5. PC-BIOS firmware
echo "==> Staging firmware and bios"
mkdir -p "$HUSK_ROOT/third_party/build/qemu-10.0.12-utm/pc-bios"
cp -R "$APP/pc-bios/"* "$HUSK_ROOT/third_party/build/qemu-10.0.12-utm/pc-bios/"
if [ -f "$APP/edk2-aarch64-code.fd" ]; then
    bzip2 -c "$APP/edk2-aarch64-code.fd" > "$HUSK_ROOT/third_party/build/qemu-10.0.12-utm/pc-bios/edk2-aarch64-code.fd.bz2"
fi

echo "==> Prebuilts successfully staged!"
