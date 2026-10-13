#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds armtr-bridge-test.apk (no Gradle): javac -> d8 -> aapt2 link -> add lib/arm64-v8a -> zipalign -> apksigner.
# Usage: build_apk.sh <libjni_bridge_test.so> <out.apk>
set -e
SO=$(realpath "$1"); OUT=$(realpath -m "$2"); HERE=${APK_DIR:-$(cd "$(dirname "$0")" && pwd)}   # APK_DIR: other manifest+src
SDK=${ANDROID_SDK:-/mnt/storage-adata/android-sdk}; BT=$SDK/build-tools/37.0.0; JAR=$SDK/platforms/android-36/android.jar
export JAVA_HOME=${JAVA_HOME:-/mnt/storage-adata/aosp15/prebuilts/jdk/jdk21/linux-x86}; export PATH=$JAVA_HOME/bin:$PATH
W=$(mktemp -d); trap 'rm -rf $W' EXIT
javac -source 11 -target 11 -nowarn -cp $JAR -d $W/classes $(find $HERE/src -name '*.java') 2>&1 | grep -v "^warning" || true
$BT/d8 --min-api 26 --lib $JAR --output $W $(find $W/classes -name '*.class')
$BT/aapt2 link -o $W/base.apk -I $JAR --manifest $HERE/AndroidManifest.xml
mkdir -p $W/z/lib/arm64-v8a && cp $SO $W/z/lib/arm64-v8a/ && cp $W/classes.dex $W/z/
(cd $W/z && zip -q -0 ../base.apk lib/arm64-v8a/$(basename $SO) && zip -q ../base.apk classes.dex)
$BT/zipalign -f -p 16 $W/base.apk $W/aligned.apk
KS=${ARMTR_KEYSTORE:-$HOME/.android/armtr-test.jks}
[ -f $KS ] || { mkdir -p $(dirname $KS); keytool -genkeypair -keystore $KS -storepass android -keypass android \
  -alias test -keyalg RSA -keysize 2048 -validity 10000 -dname CN=armtr-test >/dev/null 2>&1; }
$BT/apksigner sign --ks $KS --ks-pass pass:android --out $OUT $W/aligned.apk
echo "apk: $OUT"
