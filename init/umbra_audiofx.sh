#!/system/bin/sh
# Attach Umbra to all post-process streams at boot.
# The vendor partition is read-only, so the patched config is assembled in
# /data/vendor and init bind-mounts it over the stock one before audioserver
# starts (audio_effects.xml is read once, at audioserver startup). The copy
# keeps its umbra_data_file label; audioserver is granted read in sepolicy.

SRC=/vendor/etc/audio_effects.xml
OUT=/data/vendor/umbra/audio_effects.xml

STREAMS="music patch rerouting voice_call ring alarm notification assistant"

cp -f "$SRC" "$OUT.tmp" || exit 1

for _s in $STREAMS; do
    if grep -q "<stream type=\"$_s\">" "$OUT.tmp"; then
        sed -i "/<stream type=\"$_s\">/a\\            <apply effect=\"umbra\"/>" "$OUT.tmp"
    else
        sed -i "/<postprocess>/a\\        <stream type=\"$_s\">\n            <apply effect=\"umbra\"/>\n        </stream>" "$OUT.tmp"
    fi
done

chmod 0644 "$OUT.tmp"
mv -f "$OUT.tmp" "$OUT"
