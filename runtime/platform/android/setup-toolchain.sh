#!/bin/bash
# The Android toolchain: SDL3 built for arm64-v8a (Android 26+), the Java classes SDLActivity needs, and nothing else --
# the NDK's clang is the compiler, the SDK's build-tools make the APK (android/build-apk.sh). Into
# $ATIV_ANDROID_TOOLCHAIN (default ~/Library/Android/sdk/sdl3-android on macOS, ~/Android/Sdk/sdl3-android on Linux),
# where runtime/build.py looks first.
#
# Needs: the NDK (ANDROID_NDK_HOME, r26+), cmake, ninja. The APK step also needs a JDK (JAVA_HOME) and the SDK's
# platform-tools, platforms;android-35 and build-tools;35.0.0.
#   setup-toolchain.sh
#   ANDROID_NDK_HOME=~/Library/Android/sdk/ndk/27.2.12479018 ./setup-toolchain.sh
set -euo pipefail
SDL=3.4.16
SDL_SHA=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
API=${ANDROID_API:-26}
if [[ -z ${ANDROID_NDK_HOME:-} && -z ${ANDROID_NDK_ROOT:-} ]]; then
  echo "set ANDROID_NDK_HOME to the NDK's folder (e.g. ~/Library/Android/sdk/ndk/27.2.12479018)" >&2; exit 1
fi
NDK=${ANDROID_NDK_HOME:-$ANDROID_NDK_ROOT}
if [[ $(uname -s) == Darwin ]]; then DEF=$HOME/Library/Android/sdk/sdl3-android; else DEF=$HOME/Android/Sdk/sdl3-android; fi
D=${ATIV_ANDROID_TOOLCHAIN:-$DEF}
mkdir -p "$D/dl"; cd "$D/dl"
verify() { local h; h=$(sha256sum "$1" 2>/dev/null | awk '{print $1}'); [[ -n $h ]] || h=$(shasum -a 256 "$1" | awk '{print $1}'); [[ $h == "$2" ]] && return; echo "checksum mismatch: $1 (expected $2, got $h)" >&2; rm -f "$1"; exit 1; }
fetch() { [[ -f $2 ]] || { curl -fL --retry 3 -o "$2.part" "$1" && verify "$2.part" "$3" && mv -f "$2.part" "$2"; }; }
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz" "SDL3-$SDL.tar.gz" "$SDL_SHA"
cd "$D"
[[ -d "SDL3-$SDL" ]] || tar xzf "dl/SDL3-$SDL.tar.gz"
# the Java side of SDL's Android backend (zlib licence, in the source's android-project) goes with the toolchain: the APK
# compiles it, and no game code or SDL source is in this repository
rm -rf java; mkdir -p java
cp -R "SDL3-$SDL/android-project/app/src/main/java/org" java/
# the C side: a shared libSDL3.so the APK's jniLibs carries, Vulkan and AAudio among its backends
PATH=/opt/homebrew/bin:$PATH cmake -S "SDL3-$SDL" -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$API" -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$D"
PATH=/opt/homebrew/bin:$PATH cmake --build build-android -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
PATH=/opt/homebrew/bin:$PATH cmake --install build-android
echo "Android SDL3 $SDL -> $D (libSDL3.so, include/, java/)"
