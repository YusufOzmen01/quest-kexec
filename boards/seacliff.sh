# Board descriptor: Quest Pro (seacliff), Qualcomm SM8250 (Kona).
# Sourced by the kexec tools via tools/board.sh. See docs/MULTI_BOARD.md.

QKX_BOARD=seacliff                                   # matches ro.product.device
QKX_BOARD_LABEL="Quest Pro (seacliff)"
QKX_USB_MANUFACTURER=quest-pro-kexec
QKX_SYNCBOSS_SPI=spi0.0

QKX_NUX_APK=FirstTimeNuxSeacliff
QKX_KERNEL_LOCALVERSION=-g49638c7a8637

# First-boot DTB prep. On Kona (seacliff, hollywood) these nodes come from the
# shared kona.dtsi and are identical, and the install flow still applies them
# directly; they live here as the extension point for a future non-Kona board.
# See docs/MULTI_BOARD.md.
QKX_DISABLE_NODES="/soc/qseecom@82400000"   # status=disabled (VIDC stays on for Venus)
QKX_NOMAP_NODES="/reserved-memory/cont_splash_region@9c000000 /reserved-memory/secure_display_region"
