# Android

Nativ's runtime builds as an Android shared object (`TARGET=android`), and this folder turns it into a sideloadable
APK for arm64 devices. It is new and without a supported game's pack yet, so expect bring-up work.

## What you need

- Android NDK r26+ (`ANDROID_NDK_HOME`), the SDK with `platforms;android-35` and `build-tools;35.0.0`
  (`ANDROID_SDK_ROOT`), and a JDK (`JAVA_HOME`; 17 or 21).
- The Android SDL3 toolchain: `runtime/platform/android/setup-toolchain.sh`.
- A title built for Android:

```sh
export ANDROID_NDK_HOME=~/Library/Android/sdk/ndk/27.2.12479018
export JAVA_HOME=/opt/homebrew/opt/openjdk@21
TARGET=android ./vitarecomp build PCSB00296
./android/make-apk.py PCSB00296
```

The APK is written to `android/build/<title>.apk`. It carries the game's recompiled code, SDL3 and the memory images.

## The game's data

The game's own data is gigabytes, so it is not in the APK. Copy `fs/app0` from the library
(`dump/decrypted/<title>/fs/app0`, or the per-user library) to the app's external files folder:

```sh
adb install -r android/build/PCSB00296.apk
adb push "$HOME/Library/Application Support/ProjectNativ/Nativ/library/decrypted/PCSB00296/fs/app0" \
  /sdcard/Android/data/org.projectnativ.pcsb00296/files/fs/
```

`make-apk.py` prints the exact paths for the title it built. The app's log is in logcat under `Nativ` and `SDL`.

## Notes

- One APK per title; the package is `org.projectnativ.<title id in lower case>`.
- The window is SDL's; a gamepad works through SDL's normal mapping. Touch is SDL's, not a bespoke overlay.
- 4:3 screens (1280×960) need the game rendered 4:3: the port's display work, not a build flag. Until then the
  overlay's `fill` aspect mode uses the whole screen by cropping, and `fit` letterboxes.
- No FFmpeg on Android yet (movies), and no libcurl (online play): those calls stay stubs.
