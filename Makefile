# stealo: plain-C Android app, no Gradle. `make run` builds, installs, launches.
CLANG     := /usr/lib/llvm-19/bin/clang
STRIP     := /usr/lib/llvm-19/bin/llvm-strip
TARGET    := aarch64-linux-android29
JNI_INC   := -I. -I/usr/lib/jvm/default-java/include -I/usr/lib/jvm/default-java/include/linux
CFLAGS    := -Os -ffreestanding -fno-stack-protector -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -fvisibility=hidden $(JNI_INC)
FRAMEWORK := /usr/share/android-framework-res/framework-res.apk
D8        := java -cp tmp/tools/r8.jar com.android.tools.r8.D8
# javac: only -target 8 still allows -bootclasspath; d8 desugars 8 fine
JAVACFLAGS := -Xlint:-options -source 8 -target 8 -bootclasspath tmp/tools/android.jar

APK  := build/stealo.apk
LIB  := build/apk/lib/arm64-v8a/libstealo.so
DEX  := build/apk/classes.dex
KEY  := build/debug.keystore

# stub .so's satisfy the linker; the phone's dynamic linker binds the real
# bionic libs at runtime (stub_*.c hold only the symbol names we reference)
STUBS := build/stub/libc.so build/stub/libandroid.so build/stub/liblog.so

all: $(APK)

build/stub/lib%.so: stub_%.c
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) -shared -nostdlib -fuse-ld=lld \
	    -Wl,-soname,lib$*.so -o $@ $<

# native sources: UI/JNI core, BLE transport, protocol driver, self-contained crypto
SRC := main.c plot.c dexble.c dexdriver.c \
       dexauth.c dexdata.c p256.c sha256.c aes.c

$(LIB): $(SRC) $(STUBS)
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) $(CFLAGS) -shared -nostdlib -fuse-ld=lld \
	    -Wl,--no-undefined -Lbuild/stub -lc -landroid -llog -o $@ $(SRC)
	$(STRIP) $@

build/classes/com/jk/stealo/Ble.class: Ble.java StealoService.java Alarm.java
	javac $(JAVACFLAGS) -d build/classes Ble.java StealoService.java Alarm.java

$(DEX): build/classes/com/jk/stealo/Ble.class
	@mkdir -p $(@D)
	$(D8) --release --min-api 29 --lib tmp/tools/android.jar --output $(@D) \
	    build/classes/com/jk/stealo/*.class

$(KEY):
	keytool -genkeypair -keystore $@ -alias debug -keyalg RSA -keysize 2048 \
	    -validity 10000 -storepass android -keypass android -dname CN=debug

$(APK): AndroidManifest.xml $(LIB) $(DEX) $(KEY)
	aapt package -f -M AndroidManifest.xml -I $(FRAMEWORK) -F build/stealo.unaligned.apk
	cd build/apk && aapt add ../stealo.unaligned.apk lib/arm64-v8a/libstealo.so classes.dex
	zipalign -f -p 4 build/stealo.unaligned.apk $@
	apksigner sign --ks $(KEY) --ks-pass pass:android $@

install: $(APK)
	adb install -r $(APK)

run: install
	adb shell am start -n com.jk.stealo/android.app.NativeActivity

uninstall:
	adb uninstall com.jk.stealo

clean:
	rm -rf build

.PHONY: all install run uninstall clean
