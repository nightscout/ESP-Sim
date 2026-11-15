# Portable C, no dependencies: the Medtrum patch's framing and crypto.
idf_component_register(
    SRCS
        src/crc8.c
        src/crypto.c
        src/delivery.c
        src/frame.c
        src/session.c
        src/status.c
    INCLUDE_DIRS include
)
