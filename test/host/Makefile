# Host tests for the portable pod core. The crypto is mbedTLS, as on the
# board: built here from ESP-IDF's copy.
#   make -C test/host        builds and runs them
#   make -C test/host medtrum  the Medtrum patch core on its own (no mbedTLS)
CORE := ../../components/dashpod
CGM := ../../components/cgmsim
IDF_PATH ?= $(HOME)/esp/esp-idf
MBED := $(IDF_PATH)/components/mbedtls/mbedtls
OUT := ../../.cache/host-mbedtls

SRCS := $(wildcard $(CORE)/src/*.c) $(wildcard $(CGM)/src/*.c) host_io.c test_main.c test_o5.c test_cgm.c
CFLAGS ?= -O1 -g -fsanitize=address,undefined
WARN := -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -I$(CORE)/include -I$(CGM)/include -I. -I$(MBED)/include

MBED_SRCS := $(wildcard $(MBED)/library/*.c)
MBED_OBJS := $(patsubst $(MBED)/library/%.c,$(OUT)/%.o,$(MBED_SRCS))

test: dash_tests
	./dash_tests

dash_tests: $(SRCS) $(wildcard $(CORE)/include/*.h) $(wildcard $(CGM)/include/*.h) $(wildcard golden_*.h) host_io.h $(OUT)/libmbedcrypto.a
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS) $(OUT)/libmbedcrypto.a -lpthread

$(OUT)/%.o: $(MBED)/library/%.c
	@mkdir -p $(OUT)
	$(CC) -O2 -I$(MBED)/include -I$(MBED)/library -c $< -o $@

$(OUT)/libmbedcrypto.a: $(MBED_OBJS)
	ar rcs $@ $^

# The Medtrum patch core is portable C: no mbedTLS and no ESP-IDF. Its test
# builds on its own so it never links the pod's crypto.
MED := ../../components/medtrumpatch
MED_SRCS := $(wildcard $(MED)/src/*.c) test_medtrum.c
MED_CFLAGS := -O1 -g -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I$(MED)/include -I.

medtrum: medtrum_tests
	./medtrum_tests

medtrum_tests: $(MED_SRCS) $(wildcard $(MED)/include/*.h) test.h
	$(CC) $(MED_CFLAGS) -o $@ $(MED_SRCS)

clean:
	rm -f dash_tests medtrum_tests

.PHONY: test medtrum clean
