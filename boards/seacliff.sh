# Board descriptor: Quest Pro (seacliff), Qualcomm SM8250 (Kona).
# Sourced by the kexec tools via tools/board.sh. See docs/MULTI_BOARD.md.

QKX_BOARD=seacliff                                   # matches ro.product.device
QKX_BOARD_LABEL="Quest Pro (seacliff)"
QKX_USB_MANUFACTURER=quest-pro-kexec
QKX_SYNCBOSS_SPI=spi0.0

# Continuous-splash reserved region, marked no-map in the target DTB at prep time.
QKX_SPLASH_NODE=cont_splash_region@9c000000
QKX_SPLASH_BASE=0x9c000000
QKX_SPLASH_SIZE=0x2300000

# /soc nodes disabled for a first boot.
QKX_DISABLE_NODES="/soc/qseecom@82400000 /soc/qcom,vidc@aa00000"

QKX_NUX_APK=FirstTimeNuxSeacliff
QKX_KERNEL_LOCALVERSION=-g49638c7a8637
