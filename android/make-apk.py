#!/usr/bin/env python3
"""make-apk.py <title>: package a title built with TARGET=android into an installable APK.

  TARGET=android ./vitarecomp build PCSB00296
  ./android/make-apk.py PCSB00296

The APK carries the recompiled code (libmain.so), SDL3 and the C++ runtime for arm64-v8a, and the game's memory images
in assets (unpacked to the app's internal storage on first start). The game's own data -- fs/app0, gigabytes -- stays
the player's: copy it to /sdcard/Android/data/<package>/files/fs/app0 (or push it with adb), and the app reads it there.
Needs ANDROID_SDK_ROOT (or ANDROID_HOME), ANDROID_NDK_HOME, ATIV_ANDROID_TOOLCHAIN and a JDK (JAVA_HOME).
"""
import json
import os
import shutil
import subprocess
import sys
import zipfile
from html import escape

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "pipeline"))
import datadir  # noqa: E402
import packconf  # noqa: E402


def die(msg):
    sys.stderr.write(msg + "\n")
    sys.exit(1)


def newest_dir(parent, pred=lambda n: True):
    if not os.path.isdir(parent):
        return None
    ds = [os.path.join(parent, d) for d in os.listdir(parent) if pred(d) and os.path.isdir(os.path.join(parent, d))]
    return sorted(ds, key=lambda d: [int(x) for x in os.path.basename(d).replace("-", ".").split(".") if x.isdigit()] or [0])[-1] if ds else None


def find_sdk():
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not sdk:
        sdk = os.path.expanduser("~/Library/Android/sdk" if sys.platform == "darwin" else "~/Android/Sdk")
    if not os.path.isdir(sdk):
        die("no Android SDK: set ANDROID_SDK_ROOT")
    return sdk


def find_java():
    jh = os.environ.get("JAVA_HOME")
    if jh and os.path.isfile(os.path.join(jh, "bin", "javac")):
        return os.path.join(jh, "bin")
    if os.path.isdir("/opt/homebrew/opt/openjdk@21/bin"):
        return "/opt/homebrew/opt/openjdk@21/bin"
    jc = shutil.which("javac")
    if jc:
        return os.path.dirname(jc)
    die("no JDK: set JAVA_HOME")


def run(argv, **kw):
    r = subprocess.run(argv, **kw)
    if r.returncode:
        die("failed: " + " ".join(argv))


def main():
    if len(sys.argv) != 2:
        die(__doc__)
    title = sys.argv[1]
    lib = datadir.library_dir(ROOT)
    W = os.path.join(lib, "decrypted", title)
    pack_path = os.path.join(ROOT, "packs", title, "pack.toml")
    if not os.path.isfile(pack_path):
        die(f"{title}: no pack at packs/{title}/pack.toml")
    version = open(os.path.join(W, "version")).read().strip() if os.path.isfile(os.path.join(W, "version")) else ""
    c = packconf.load(pack_path, version)
    base = c["BASE"]
    mods = c["MODULES"].split()
    name = c["NAME"]

    entry_path = os.path.join(W, "analysis", "eboot.data.json")
    if not os.path.isfile(entry_path):
        die(f"{title}: no analysis: ./vitarecomp analyze {title}")
    entry = json.load(open(entry_path))["functions"]["module_start"] & ~1

    exe = os.path.join(W, "recomp", base + "_host.so")
    if not os.path.isfile(exe):
        die(f"{title}: no Android build: TARGET=android ./vitarecomp build {title}\n  (expected {exe})")

    sdk = find_sdk()
    bt = newest_dir(os.path.join(sdk, "build-tools"))
    platform = newest_dir(os.path.join(sdk, "platforms"), lambda d: d.startswith("android-"))
    if not bt or not platform:
        die("the SDK needs build-tools and a platform (sdkmanager 'build-tools;35.0.0' 'platforms;android-35')")
    android_jar = os.path.join(platform, "android.jar")
    java = find_java()
    os.environ["PATH"] = java + os.pathsep + os.environ.get("PATH", "")   # d8 and apksigner are scripts that call java

    ndk = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT")
    if not ndk or not os.path.isdir(ndk):
        die("set ANDROID_NDK_HOME to the NDK's folder")
    ndk_lib = os.path.join(ndk, "toolchains", "llvm", "prebuilt",
                           "darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64",
                           "sysroot", "usr", "lib", "aarch64-linux-android")
    at = os.environ.get("ATIV_ANDROID_TOOLCHAIN") or os.path.expanduser(
        "~/Library/Android/sdk/sdl3-android" if sys.platform == "darwin" else "~/Android/Sdk/sdl3-android")
    if not os.path.isfile(os.path.join(at, "lib", "libSDL3.so")):
        die(f"no Android SDL3 at {at}: runtime/platform/android/setup-toolchain.sh")

    pkg = "org.projectnativ." + title.lower()
    bd = os.path.join(HERE, "build", title)
    if os.path.isdir(bd):
        shutil.rmtree(bd)
    assets = os.path.join(bd, "assets", "images")
    src = os.path.join(bd, "src", *pkg.split("."))
    classes = os.path.join(bd, "classes")
    libs = os.path.join(bd, "lib", "arm64-v8a")
    for d in (assets, src, classes, libs):
        os.makedirs(d, exist_ok=True)

    # assets: the memory images (the game's and the Sony modules') and the pack's shader recipes, if it has any
    shutil.copyfile(os.path.join(W, "recomp", base + ".img"), os.path.join(assets, base + ".img"))
    for m in mods:
        p = os.path.join(W, "recomp", m + ".img")
        if not os.path.isfile(p):
            die(f"{title}: no module image {p}: run ./vitarecomp recompile {title}")
        shutil.copyfile(p, os.path.join(assets, "mod_" + m + ".img"))
    shaders = os.path.join(ROOT, "packs", title, "shaders.txt")
    has_shaders = os.path.isfile(shaders)
    if has_shaders:
        shutil.copyfile(shaders, os.path.join(bd, "assets", "shaders.txt"))

    # native libraries: the game, SDL3, and the C++ runtime ImGui and the overlay use
    shutil.copyfile(exe, os.path.join(libs, "libmain.so"))
    shutil.copyfile(os.path.join(at, "lib", "libSDL3.so"), os.path.join(libs, "libSDL3.so"))
    cxx = os.path.join(ndk_lib, "libc++_shared.so")
    if os.path.isfile(cxx):
        shutil.copyfile(cxx, os.path.join(libs, "libc++_shared.so"))

    # manifest and activity
    man = open(os.path.join(HERE, "AndroidManifest.xml.in")).read()
    man = man.replace("@PKG@", pkg).replace("@NAME@", escape(name))
    open(os.path.join(bd, "AndroidManifest.xml"), "w").write(man)
    act = open(os.path.join(HERE, "NativActivity.java.in")).read()
    mods_java = ", ".join(json.dumps(m) for m in mods)
    act = (act.replace("@PKG@", pkg).replace("@BASE@", base).replace("@ENTRY@", hex(entry))
              .replace("@TITLE_ID@", title).replace("@GAME_NAME@", name.replace('"', '\\"'))
              .replace("@MODULES@", mods_java).replace("@SHADERS@", "true" if has_shaders else "false"))
    open(os.path.join(src, "NativActivity.java"), "w").write(act)

    # Java (SDL's classes from the toolchain + ours), then dex
    jfiles = [os.path.join(src, "NativActivity.java")]
    for root, _, files in os.walk(os.path.join(at, "java")):
        jfiles += [os.path.join(root, f) for f in files if f.endswith(".java")]
    run([os.path.join(java, "javac"), "-source", "17", "-target", "17", "-nowarn",
         "-classpath", android_jar, "-d", classes] + jfiles)
    run([os.path.join(bt, "d8"), "--lib", android_jar, "--min-api", "26",
         "--output", bd] + [os.path.join(root, f) for root, _, files in os.walk(classes) for f in files if f.endswith(".class")])

    # resources (none) and the manifest into the base APK, then the dex and the unpacked native libraries
    apk = os.path.join(bd, "unsigned.apk")
    run([os.path.join(bt, "aapt2"), "link", "-o", apk, "-I", android_jar,
         "--manifest", os.path.join(bd, "AndroidManifest.xml"), "-A", os.path.join(bd, "assets"),
         "--min-sdk-version", "26", "--target-sdk-version", "35", "--version-code", "1", "--version-name", "1.0"])
    with zipfile.ZipFile(apk, "a", zipfile.ZIP_STORED) as z:
        z.write(os.path.join(bd, "classes.dex"), "classes.dex")
        for f in sorted(os.listdir(libs)):
            z.write(os.path.join(libs, f), "lib/arm64-v8a/" + f)

    aligned = os.path.join(bd, "aligned.apk")
    run([os.path.join(bt, "zipalign"), "-P", "16", "-f", "4", apk, aligned])

    ks = os.path.join(HERE, "debug.keystore")
    if not os.path.isfile(ks):
        run([os.path.join(java, "keytool"), "-genkeypair", "-keystore", ks, "-storepass", "android",
             "-keypass", "android", "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"])
    out = os.path.join(HERE, "build", title + ".apk")
    run([os.path.join(bt, "apksigner"), "sign", "--ks", ks, "--ks-pass", "pass:android",
         "--key-pass", "pass:android", "--out", out, aligned])

    print(f"\n{out}")
    print(f"  package: {pkg}")
    print(f"  game data: /sdcard/Android/data/{pkg}/files/fs/app0")
    print(f"  adb: adb install -r '{out}'")
    print(f"       adb push '{os.path.join(W, 'fs', 'app0')}' /sdcard/Android/data/{pkg}/files/fs/")


if __name__ == "__main__":
    main()
