# Board descriptor: Quest 2 (hollywood), Qualcomm SM8250 (Kona).
# Sourced by the kexec tools via tools/board.sh. See docs/MULTI_BOARD.md.
#
# Partial: the device-specific DTB values are TBD until a live Quest 2 capture.

QKX_BOARD=hollywood                                  # matches ro.product.device
QKX_BOARD_LABEL="Quest 2 (hollywood)"
QKX_USB_MANUFACTURER=quest2-kexec
QKX_SYNCBOSS_SPI=spi1.0

# TBD from a live capture (see docs/MULTI_BOARD.md, "Still open for hollywood"):
QKX_SPLASH_NODE=        # cont_splash_region@<base>
QKX_SPLASH_BASE=        # 0x...
QKX_SPLASH_SIZE=        # 0x...
QKX_DISABLE_NODES=      # "/soc/qseecom@<addr> /soc/qcom,vidc@<addr>"
QKX_NUX_APK=            # FirstTimeNux<...>

QKX_KERNEL_LOCALVERSION=-gca1c761b6064
