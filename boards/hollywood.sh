# Board descriptor: Quest 2 (hollywood), Qualcomm SM8250 (Kona).
# Sourced by the kexec tools via tools/board.sh. See docs/MULTI_BOARD.md.
#
# Complete except for the first-time-setup APK name (TBD).

QKX_BOARD=hollywood                                  # matches ro.product.device
QKX_BOARD_LABEL="Quest 2 (hollywood)"
QKX_USB_MANUFACTURER=quest2-kexec
QKX_SYNCBOSS_SPI=spi1.0

QKX_NUX_APK=            # TBD: hollywood first-time-setup APK name
QKX_KERNEL_LOCALVERSION=-gca1c761b6064

# Same Kona SoC as seacliff: these nodes come from the shared kona.dtsi with no
# board override, so the values match seacliff. See docs/MULTI_BOARD.md.
QKX_DISABLE_NODES="/soc/qseecom@82400000"
QKX_NOMAP_NODES="/reserved-memory/cont_splash_region@9c000000 /reserved-memory/secure_display_region"
