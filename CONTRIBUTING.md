# Contributing

Keep hardware/OS specifics explicit in commit messages. Do not commit boot
images, firmware, device captures, serial numbers, vbmeta digests, proprietary
binaries, or files pulled from partitions. Generated payloads belong under
`out/` and are ignored.

A hardware test report should state the Android build/kernel, target commit,
loader parameters, target command line additions, power/cable state, and whether
retained/pstore evidence survived.
