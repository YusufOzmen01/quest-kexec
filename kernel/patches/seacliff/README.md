# seacliff (Quest Pro) — no override

The common base series (`../0001`–`../0003`) already targets seacliff: it sets
`QKX_LOG_PHYS = 0x9ba80000`, which is correct for the Quest Pro ramoops layout.
So there is **no seacliff-specific patch** to apply — just the common base.

This folder exists to make the per-device split explicit and symmetric with
`../hollywood/`. If a future seacliff-only divergence appears, its patch goes here.

Remember to build `module/marker_read.ko` with `QKX_LOG_PHYS = 0x9ba80000` for a
seacliff target (see `../README.md`); the checked-in default is the Quest 2 value.
