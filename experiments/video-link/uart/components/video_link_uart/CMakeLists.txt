# C5 sender of the UART video link: frame grabber on the receiver's raw I/Q
# ring (grab.c), row codec and sender (link_tx.c), UART transport
# (uart_link.c), stream task, scan and console keys (link.c). It implements
# video_link_start() / video_link_console(), which main/video.c calls in a
# build that defines C5VRX_VIDEO_LINK_EXPERIMENT (../../CMakeLists.txt).
#
# WHOLE_ARCHIVE: main/ calls into this component without listing it as a
# requirement (the receiver stays independent of the experiment), so the
# linker must keep these objects whatever the library order.
idf_component_register(
    SRCS
        "link.c"
        "grab.c"
        "link_tx.c"
        "uart_link.c"
    INCLUDE_DIRS "." "../../protocol"
    REQUIRES main
    PRIV_REQUIRES
        esp_driver_uart
        esp_driver_bitscrambler
        esp_timer
    WHOLE_ARCHIVE
)

# Loopback twin of the Phase5 demodulator for the 'v' dump (debug build).
target_bitscrambler_add_src("fm_loop.bsasm")

# The frame grabber runs per-sample loops against the 40 MS/s ring and the
# link sender encodes rows inside the grab loop; the project default (-Og) is
# several times too slow for them.
set_source_files_properties(grab.c link_tx.c PROPERTIES COMPILE_OPTIONS "-O2")
