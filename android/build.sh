#!/bin/bash
# Build a title for Android and package it: ./android/build.sh PCSB00296
# Env, when the defaults aren't right: ANDROID_NDK_HOME, ATIV_ANDROID_TOOLCHAIN, JAVA_HOME. JOBS for the C compiler.
set -euo pipefail
T=${1:?usage: android/build.sh <title>}
cd "$(dirname "$0")/.."

if [[ -z ${ANDROID_NDK_HOME:-} ]]; then
  for d in "$HOME/Library/Android/sdk/ndk"/* "$HOME/Android/Sdk/ndk"/*; do
    [[ -d $d ]] && ANDROID_NDK_HOME=$d
  done
fi
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK's folder}"
if [[ -z ${JAVA_HOME:-} && -d /opt/homebrew/opt/openjdk@21 ]]; then JAVA_HOME=/opt/homebrew/opt/openjdk@21; fi
: "${JAVA_HOME:?set JAVA_HOME to a JDK 17 or 21}"
export ANDROID_NDK_HOME JAVA_HOME
export ATIV_ANDROID_TOOLCHAIN=${ATIV_ANDROID_TOOLCHAIN:-$HOME/Library/Android/sdk/sdl3-android}
[[ -f $ATIV_ANDROID_TOOLCHAIN/lib/libSDL3.so ]] || runtime/platform/android/setup-toolchain.sh

TARGET=android ./vitarecomp build "$T"
./android/make-apk.py "$T"
