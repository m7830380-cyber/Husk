#!/bin/bash
# build.sh <out.dex> -- Husk's Java framework (android.* for apps run on Husk's Dalvik runtime): javac, then d8.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -n "${HUSK_JDK:-}" ] && [ -d "$HUSK_JDK" ]; then
    JDK="$HUSK_JDK"
elif [ -n "${JAVA_HOME:-}" ] && [ -d "$JAVA_HOME" ]; then
    JDK="$JAVA_HOME"
elif [ -d "/Volumes/GTAV/husk2/tools-dl/jdk/Contents/Home" ]; then
    JDK="/Volumes/GTAV/husk2/tools-dl/jdk/Contents/Home"
else
    JAVAC_BIN="$(command -v javac || true)"
    if [ -n "$JAVAC_BIN" ]; then
        JDK="$(cd "$(dirname "$JAVAC_BIN")/.." && pwd)"
    else
        echo "error: javac not found" >&2; exit 1
    fi
fi

if [ -n "${HUSK_D8:-}" ] && [ -f "$HUSK_D8" ]; then
    D8="$HUSK_D8"
elif [ -f "/Volumes/GTAV/husk2/tools-dl/r8.jar" ]; then
    D8="/Volumes/GTAV/husk2/tools-dl/r8.jar"
else
    D8_DIR="$HERE/../../third_party/tools"
    mkdir -p "$D8_DIR"
    D8="$D8_DIR/r8.jar"
    if [ ! -f "$D8" ]; then
        echo "==> downloading r8.jar"
        curl -fL --retry 3 -o "$D8" "https://dl.google.com/dl/android/maven2/com/android/tools/r8/8.5.35/r8-8.5.35.jar"
    fi
fi
OUT="${1:-$HERE/../../build/husk-framework.dex}"
TMP="$(mktemp -d)"
python3 "$HERE/gen/gles20.py" "$HERE/src/android/opengl" "$HERE/../translation-layer-next/husk-tl-dvm-gles20.c" >/dev/null
# libcore's own classes that javac's JDK does not have (org.xmlpull): compiled to build against, not put in the dex
"$JDK/bin/javac" -nowarn --release 8 -encoding UTF-8 -d "$TMP/stubs" $(find "$HERE/stubs" -name '*.java')
if ! "$JDK/bin/javac" -nowarn --release 8 -encoding UTF-8 -Xmaxerrs 100000 -cp "$TMP/stubs" -d "$TMP/classes" $(find "$HERE/src" "$HERE/gen-src" -name '*.java') 2> "$TMP/javac.log"; then
    grep -v "^Note:" "$TMP/javac.log" > "${JAVAC_LOG:-/dev/stderr}"; echo "javac failed" >&2; exit 1
fi
(cd "$TMP/classes" && "$JDK/bin/jar" cf "$TMP/classes.jar" .)
"$JDK/bin/java" -cp "$D8" com.android.tools.r8.D8 --min-api 26 --output "$TMP" "$TMP/classes.jar"
mkdir -p "$(dirname "$OUT")"
mv "$TMP/classes.dex" "$OUT"

# Core dex: handwritten src/ only (and nested types of those classes). Generated
# gen-src stubs stay out so AOSP framework.jar can define them when it is on the
# boot path. Nested stubs of a handwritten outer class stay here so Activity$Foo
# still matches Husk's Activity.
CORE="${OUT%.dex}-core.dex"
# core_dex <keep-list or ""> <out.dex>: the handwritten classes, plus every generated one not on the list of classes AOSP replaces
core_dex() {
rm -rf "$TMP/core-classes" "$TMP/core-out"
python3 - "$HERE/src" "$TMP/classes" "$TMP/core-classes" "$1" <<'PY'
import os, shutil, sys
src, classes, dest, aosp = sys.argv[1:5]
replaced = set(l.strip() for l in open(aosp)) if aosp else None
os.makedirs(dest, exist_ok=True)
kept = 0
for dirpath, _, files in os.walk(classes):
    for name in files:
        if not name.endswith(".class"):
            continue
        rel = os.path.relpath(os.path.join(dirpath, name), classes)
        outer = rel[:-6].split("$", 1)[0]
        if os.path.isfile(os.path.join(src, outer + ".java")) or (replaced is not None and "L" + rel[:-6] + ";" not in replaced):
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copy2(os.path.join(dirpath, name), out)
            kept += 1
print(kept)
PY
(cd "$TMP/core-classes" && "$JDK/bin/jar" cf "$TMP/core.jar" .)
mkdir -p "$TMP/core-out"
"$JDK/bin/java" -cp "$D8" com.android.tools.r8.D8 --min-api 26 --output "$TMP/core-out" "$TMP/core.jar"
mv "$TMP/core-out/classes.dex" "$2"
}
core_dex "" "$CORE"
# HUSK_AOSP_JARS (framework.jar and friends, space-separated): the core dex keeps the generated stand-ins AOSP can not replace
# (tools/compat/aosppure.py), so on the AOSP boot path a class comes from AOSP's bytecode wherever that works without Husk
if [ -n "${HUSK_AOSP_JARS:-}" ]; then
    python3 "$HERE/../../tools/compat/aosppure.py" "$OUT" "$CORE" $HUSK_AOSP_JARS --out="$TMP/aosp-replaced.txt"
    core_dex "$TMP/aosp-replaced.txt" "$CORE"
fi
rm -rf "$TMP"
echo "built $OUT"
echo "built $CORE"
