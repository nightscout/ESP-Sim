idf_component_register(
    SRCS src/cgm.c src/person.c src/backtest.c
    INCLUDE_DIRS include
    PRIV_REQUIRES dashpod
)
