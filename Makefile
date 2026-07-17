# SPDX-License-Identifier: GPL-3.0
# stealo: plain-C Android app, no Gradle. `make run` builds, installs, launches.
CLANG     := /usr/lib/llvm-19/bin/clang
STRIP     := /usr/lib/llvm-19/bin/llvm-strip
TARGET    := aarch64-linux-android29
JNI_INC   := -Isrc -I/usr/lib/jvm/default-java/include -I/usr/lib/jvm/default-java/include/linux
# Every warning on, warnings are errors. Keep this list in sync with what the
# code actually satisfies — the rule is to fix the cause, never to silence.
WARN      := -Werror -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
             -Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2 -Wundef \
             -Wdouble-promotion -Wcast-qual -Wswitch-enum -Wredundant-decls
CFLAGS    := -Os -ffreestanding -fno-stack-protector -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -fvisibility=hidden $(WARN) $(JNI_INC)
FRAMEWORK := /usr/share/android-framework-res/framework-res.apk
D8        := java -cp tmp/tools/r8.jar com.android.tools.r8.D8
# javac: only -target 8 still allows -bootclasspath; d8 desugars 8 fine
JAVACFLAGS := -Xlint:-options -source 8 -target 8 -bootclasspath tmp/tools/android.jar

APK  := build/stealo.apk
LIB  := build/apk/lib/arm64-v8a/libstealo.so
DEX  := build/apk/classes.dex
# kept OUTSIDE build/ so `make clean` can't wipe it — a regenerated key changes
# the APK signature and blocks in-place updates (forcing an uninstall/data loss)
KEY  := tmp/debug.keystore

# stub .so's satisfy the linker; the phone's dynamic linker binds the real
# bionic libs at runtime (stub_*.c hold only the symbol names we reference)
STUBS := build/stub/libc.so build/stub/libandroid.so build/stub/liblog.so

# launcher icon + any future resources (compiled into the APK via aapt -S)
RES := $(wildcard res/*/*.xml)

# The manifest ships release-safe (no android:debuggable). Dev builds flip it on
# with aapt --debug-mode so crash.log is retrievable on-device; `make release`
# overrides this to empty for a debuggable=false artifact.
AAPT_DEBUG := --debug-mode

all: $(APK)

build/stub/lib%.so: src/stub_%.c
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) -Isrc -shared -nostdlib -fuse-ld=lld \
	    -Wl,-soname,lib$*.so -o $@ $<

# native sources: UI/JNI core, BLE transport, protocol driver, self-contained crypto
SRC := src/main.c src/font.c src/plot.c src/util.c src/stats.c src/store.c src/settings.c src/ui.c \
       src/dexble.c src/dexdriver.c \
       src/dexauth.c src/dexdata.c src/p256.c src/sha256.c src/aes.c

$(LIB): $(SRC) $(STUBS)
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) $(CFLAGS) -shared -nostdlib -fuse-ld=lld \
	    -Wl,--no-undefined -Lbuild/stub -lc -landroid -llog -o $@ $(SRC)
	$(STRIP) $@

build/classes/com/jk/stealo/Ble.class: src/Ble.java src/StealoService.java src/Alarm.java
	javac $(JAVACFLAGS) -d build/classes src/Ble.java src/StealoService.java src/Alarm.java

$(DEX): build/classes/com/jk/stealo/Ble.class
	@mkdir -p $(@D)
	$(D8) --release --min-api 29 --lib tmp/tools/android.jar --output $(@D) \
	    build/classes/com/jk/stealo/*.class

$(KEY):
	keytool -genkeypair -keystore $@ -alias debug -keyalg RSA -keysize 2048 \
	    -validity 10000 -storepass android -keypass android -dname CN=debug

# Mode stamp: AAPT_DEBUG isn't a file, so without this a `make` after `make
# release` (or vice-versa) would NOT rebuild the APK -- leaving the wrong
# debuggable flag installed. The stamp's contents change with the mode, forcing
# a rebuild exactly when it flips.
FORCE:
build/.aapt-mode: FORCE
	@mkdir -p $(@D); printf '%s' '$(AAPT_DEBUG)' | cmp -s - $@ 2>/dev/null \
	    || printf '%s' '$(AAPT_DEBUG)' > $@

$(APK): AndroidManifest.xml $(LIB) $(DEX) $(KEY) $(RES) build/.aapt-mode
	aapt package -f -M AndroidManifest.xml -S res -I $(FRAMEWORK) $(AAPT_DEBUG) \
	    -F build/stealo.unaligned.apk
	cd build/apk && aapt add ../stealo.unaligned.apk lib/arm64-v8a/libstealo.so classes.dex
	zipalign -f -p 4 build/stealo.unaligned.apk $@
	apksigner sign --ks $(KEY) --ks-pass pass:android $@

# Release artifact: identical build with debuggable off. Still signed with the
# local debug key -- swap in your Play upload key (apksigner --ks) before upload.
release:
	rm -f $(APK) build/stealo.unaligned.apk
	$(MAKE) AAPT_DEBUG= $(APK)
	@printf '\033[1;32mrelease\033[0m: %s built with debuggable=false.\n' "$(APK)"
	@printf '  Sign with your Play upload key before uploading.\n'

# Play App Bundle (.aab), built without Gradle: aapt2 links resources in
# protobuf format, we assemble bundletool's module layout, then build-bundle.
# debuggable is off (an .aab is a release artifact). The result is UNSIGNED --
# sign it with your upload key (jarsigner -keystore upload.jks build/stealo.aab)
# or let Play App Signing handle it. Requires tmp/tools/bundletool.jar.
AAB        := build/stealo.aab
BUNDLETOOL := java -jar tmp/tools/bundletool.jar

aab: $(AAB)
$(AAB): AndroidManifest.xml $(LIB) $(DEX) $(RES)
	@test -f tmp/tools/bundletool.jar || { echo "tmp/tools/bundletool.jar missing -- get it from https://github.com/google/bundletool/releases"; exit 1; }
	rm -rf build/aab && mkdir -p build/aab/module/manifest build/aab/module/dex build/aab/module/lib/arm64-v8a
	aapt2 compile --dir res -o build/aab/res.zip
	aapt2 link --proto-format -o build/aab/base-proto.apk -I $(FRAMEWORK) \
	    --manifest AndroidManifest.xml --min-sdk-version 29 --target-sdk-version 34 \
	    -R build/aab/res.zip --auto-add-overlay
	cd build/aab/module && unzip -qo ../base-proto.apk
	mv build/aab/module/AndroidManifest.xml build/aab/module/manifest/
	cp $(DEX) build/aab/module/dex/classes.dex
	cp $(LIB) build/aab/module/lib/arm64-v8a/
	cd build/aab/module && rm -f ../base.zip && zip -qr ../base.zip manifest dex res lib resources.pb
	$(BUNDLETOOL) build-bundle --modules=build/aab/base.zip --output=$@
	@printf '\033[1;32maab\033[0m: %s built (debuggable=false, UNSIGNED). Sign with your upload key before upload.\n' "$@"

install: $(APK)
	adb install -r $(APK)

run: install
	adb shell am start -n com.jk.stealo/android.app.NativeActivity

uninstall:
	adb uninstall com.jk.stealo

clean:
	rm -rf build

# Static code analysis
# ---------------------
# `make check` runs the same gate offline: no CRLF, ASCII-only, clang-format
# clean, and clang-tidy clean (rules in .clang-tidy, warnings-as-errors). No
# compilation database is needed — the whole app is one clang invocation, so we
# hand clang-tidy the exact compile flags after `--`.
FMT_SRC   := $(wildcard src/*.c src/*.h)
TIDY_ARGS := --target=$(TARGET) -ffreestanding $(JNI_INC)

check: format tidy done

format:
	grep -rlP '\r' --exclude='.*' src res Makefile AndroidManifest.xml \
		&& { echo "CRLF line endings found (see above)"; exit 1; } || true
	grep -rlP '[^\x00-\x7F]' $(FMT_SRC) \
		&& { echo "Non-ASCII characters found (see above)"; exit 1; } || true
	clang-format --dry-run -Werror $(FMT_SRC)

format-fix:
	clang-format -i $(FMT_SRC)

tidy:
	clang-tidy $(SRC) -- $(TIDY_ARGS)

done:
	@printf '\033[1;32mSUCCESS\033[0m: all checks passed.\n'

# offline UI render harness: builds ui.c/font.c on the host and renders the
# screens to test/*.ppm, so the UI can be verified with no phone attached.
# -iquote so "" project headers come from src/ while <> headers stay glibc's
# (the freestanding shims lack FILE/fopen etc. the harness needs on the host).
JVM_INC := -I/usr/lib/jvm/default-java/include -I/usr/lib/jvm/default-java/include/linux
uitest:
	@mkdir -p tmp/uitest
	cc -iquote src $(JVM_INC) -Wall -Wextra test/uitest.c src/ui.c src/font.c src/plot.c -o tmp/uitest/uitest
	./tmp/uitest/uitest

.PHONY: FORCE all release aab install run uninstall clean check format format-fix tidy done uitest
