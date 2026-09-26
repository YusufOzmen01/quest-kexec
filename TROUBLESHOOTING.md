# Troubleshooting

## Module refuses to load

Check:

```sh
adb shell uname -r
modinfo -F vermagic module/quest_kexec.ko
```

Rebuild against the exact Android source, config, local version and
`Module.symvers`. Do not force-load it.

## USB appears but the shell is unreachable

The ECM-only interface may have a different name than older ACM+ECM payloads.
Find it with `ip link`, then use NetworkManager or assign `10.42.0.1/24`.

## Android returns immediately

Run:

```sh
tools/read-log.sh
```

A retained log means the target reached the log ring. Pstore may instead show a
loader panic inside Android.

## No retained log

A zero marker can mean a PMIC hard reset or a failure before the target log was
initialized. Do not assume the last target driver caused it without a logged
checkpoint.

## Device locks up

Force reboot only when necessary; it usually erases the useful RAM log. Do not
repeat the same payload blindly. Add a checkpoint or narrow the boundary first.
