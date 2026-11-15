# Portable C, no dependencies: the Medtrum patch's framing and crypto.
idf_component_register(
    SRCS
        src/crc8.c
        src/crypto.c
        src/frame.c
    INCLUDE_DIRS include
)
