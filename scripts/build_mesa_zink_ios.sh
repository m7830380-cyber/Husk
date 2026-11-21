#!/bin/bash
# Build Mesa's Zink (OpenGL ES 3.2 over Vulkan) for iOS arm64, against MoltenVK, with EGL (surfaceless).
#
# ANGLE's Metal backend stops at ES 3.0 plus a partial 3.1 with no compute; Zink on MoltenVK gives a real ES 3.1/3.2
# with compute, which Unreal's ES 3.1 path needs. Output: build/ios-arm64/zink/{libEGL,libGLESv2,...}.dylib
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MESA_TAG="${MESA_TAG:-mesa-25.2.0}"
SRC="$HUSK_ROOT/third_party/mesa"
OUT="$HUSK_ROOT/build/ios-arm64/zink"
MVK="${MOLTENVK_DIR:-$HUSK_ROOT/build/ios-arm64/lib/MoltenVK.xcframework}"
mkdir -p "$HUSK_ROOT/third_party" "$OUT"

[ -d "$SRC" ] || git clone --depth 1 --branch "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos -f clang)"
CXX="$(xcrun --sdk iphoneos -f clang++)"
AR="$(xcrun --sdk iphoneos -f ar)"
STRIP="$(xcrun --sdk iphoneos -f strip)"
CROSS="$HUSK_ROOT/build/mesa-ios-cross.txt"
cat > "$CROSS" <<CF
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=15.0']
cpp = ['$CXX', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=15.0']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=15.0']
ar = '$AR'
strip = '$STRIP'
[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[built-in options]
# Generated lexers include u_endian.h under _POSIX_C_SOURCE, which hides Darwin's byte-order macros; arm64 iOS is little-endian.
# MoltenVK's mvk_vulkan.h turns on the Metal/iOS Vulkan platforms after vulkan.h is already in, so Mesa's dispatch tables see
# the guards without the types; define them up front so every translation unit includes vulkan_metal.h / vulkan_ios.h.
c_args = ['-DUTIL_ARCH_LITTLE_ENDIAN=1', '-DUTIL_ARCH_BIG_ENDIAN=0', '-DVK_USE_PLATFORM_METAL_EXT', '-DVK_USE_PLATFORM_IOS_MVK']
cpp_args = ['-DUTIL_ARCH_LITTLE_ENDIAN=1', '-DUTIL_ARCH_BIG_ENDIAN=0', '-DVK_USE_PLATFORM_METAL_EXT', '-DVK_USE_PLATFORM_IOS_MVK']
CF

# MoltenVK's ios-arm64 slice: Zink links the Vulkan loader-less ICD directly.
MVK_LIB="$(find "$MVK" -path '*ios-arm64/*' -name MoltenVK -type f | grep -v simulator | head -1)"
echo "==> MoltenVK: $MVK_LIB"

cd "$SRC"
for p in "$HUSK_ROOT"/patches/mesa/*.patch; do [ -f "$p" ] && { git apply --check "$p" 2>/dev/null && git apply "$p" && echo "applied $(basename "$p")"; } ; done

# Mesa only builds its DRI loader (which EGL's surfaceless platform runs through) on KMS/DRM systems; a Darwin build has the
# same loader code without the kernel half, so allow it there.
sed -i '' "s/^if with_gallium and system_has_kms_drm$/if with_gallium and (system_has_kms_drm or host_machine.system() == 'darwin')/" meson.build
grep -n "^if with_gallium and" meson.build

rm -rf build-ios
meson setup build-ios --cross-file "$CROSS" \
    -Dbuildtype=release -Db_ndebug=true \
    -Dgallium-drivers=zink -Dvulkan-drivers= \
    -Dplatforms= -Degl=enabled -Dgbm=disabled -Dglx=disabled \
    -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
    -Dllvm=disabled -Dshared-llvm=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
    -Dbuild-tests=false -Dmoltenvk-dir="${MOLTENVK_SDK:-$HUSK_ROOT/build/ios-arm64/lib/moltenvk-sdk}" \
    -Dprefix="$OUT" 2>&1 | tee "$HUSK_ROOT/build/mesa-configure.log"
ninja -C build-ios -k 0 2>&1 | tee "$HUSK_ROOT/build/mesa-ninja.log" | tail -80
ninja -C build-ios install 2>&1 | tail -40
find "$OUT" -name '*.dylib' -exec ls -la {} \;
