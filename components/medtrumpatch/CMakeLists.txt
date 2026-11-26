# The portable Medtrum patch core. No mbedTLS, no ESP-IDF beyond the build
# system: the platform supplies a medtrum_io_t (radio, clock, randomness).
idf_component_register(
    SRCS
        src/crc8.c
        src/crypto.c
        src/delivery.c
        src/frame.c
        src/request.c
        src/session.c
        src/status.c
    INCLUDE_DIRS include
)
