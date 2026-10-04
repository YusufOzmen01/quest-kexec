#!/dev/.qkx/bin/sh
status=FAIL
trap 'echo STATUS=$status' EXIT
SS=$(pidof system_server)
N="nsenter -t $SS -m -r/proc/$SS/root"
tr '\0' '\n' < /proc/$SS/environ | grep -E '^(ANDROID_(ROOT|DATA|ART_ROOT|I18N_ROOT|TZDATA_ROOT)|BOOTCLASSPATH|DEX2OATBOOTCLASSPATH)=' > /data/local/tmp/venus-java-env.txt
while IFS= read -r kv; do export "$kv"; done < /data/local/tmp/venus-java-env.txt
export CLASSPATH=/data/local/tmp/venus-test.dex
for i in 1 2 3; do
 echo VENUS_RUN=$i
 $N /system/bin/app_process64 /system/bin VenusEncode /data/local/tmp/venus-ram12-$i.mp4 1920 1080 300 || exit 1
 $N /system/bin/app_process64 /system/bin VenusTest /data/local/tmp/venus-ram12-$i.mp4 c2.qti.avc.decoder || exit 1
done
status=PASS
