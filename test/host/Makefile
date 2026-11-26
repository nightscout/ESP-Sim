# Host tests for the portable pod core. The crypto is mbedTLS, as on the
# board: built here from ESP-IDF's copy.
#   make -C test/host        builds and runs them
CORE := ../../components/dashpod
CGM := ../../components/cgmsim
YPSO := ../../components/ypsosim
MED := ../../components/medtrumpatch
PORTAL := ../../components/portalsim
IDF_PATH ?= $(HOME)/esp/esp-idf
MBED := $(IDF_PATH)/components/mbedtls/mbedtls
OUT := ../../.cache/host-mbedtls

SRCS := $(wildcard $(CORE)/src/*.c) $(wildcard $(CGM)/src/*.c) $(wildcard $(YPSO)/src/*.c) host_io.c test_main.c test_o5.c test_cgm.c test_ypso.c
CFLAGS ?= -O1 -g -fsanitize=address,undefined
WARN := -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -I$(CORE)/include -I$(CGM)/include -I$(YPSO)/include -I. -I$(MBED)/include

MBED_SRCS := $(wildcard $(MBED)/library/*.c)
MBED_OBJS := $(patsubst $(MBED)/library/%.c,$(OUT)/%.o,$(MBED_SRCS))

test: dash_tests
	./dash_tests

# The portable Medtrum patch core: no mbedTLS and no ESP-IDF, so this compiles
# only components/medtrumpatch and the Medtrum tests with plain cc.
#   make -C test/host medtrum
MED_CFLAGS := -O1 -g -fsanitize=address,undefined -std=c11 -D_DEFAULT_SOURCE $(WARN) -I$(MED)/include -I.

medtrum: medtrum_tests draw_tests serial_tests
	./medtrum_tests
	./draw_tests
	./serial_tests

# The core byte vectors, the delivery draw and the serial/model helpers. The
# serial test pins main/medtrum_serial.h, the same header the panel uses.
medtrum_tests: test_medtrum.c $(wildcard $(MED)/src/*.c) $(wildcard $(MED)/include/*.h) test.h
	$(CC) $(MED_CFLAGS) -o $@ test_medtrum.c $(wildcard $(MED)/src/*.c)

draw_tests: test_draw.c $(wildcard $(MED)/src/*.c) $(wildcard $(MED)/include/*.h) test.h
	$(CC) $(MED_CFLAGS) -o $@ test_draw.c $(wildcard $(MED)/src/*.c)

serial_tests: test_serial.c $(wildcard $(MED)/include/*.h) ../../main/medtrum_serial.h
	$(CC) $(MED_CFLAGS) -I../../main -o $@ test_serial.c $(wildcard $(MED)/src/*.c)

# The captive-portal decision: portable C and no mbedTLS, so plain cc.
#   make -C test/host portal
PORTAL_CFLAGS := -O1 -g -fsanitize=address,undefined -std=c11 -D_DEFAULT_SOURCE $(WARN) -I$(PORTAL)/include -I.

portal: portal_tests
	./portal_tests

portal_tests: test_portal.c $(wildcard $(PORTAL)/src/*.c) $(wildcard $(PORTAL)/include/*.h) test.h
	$(CC) $(PORTAL_CFLAGS) -o $@ test_portal.c $(wildcard $(PORTAL)/src/*.c)

# The captive portal's DNS answer builder: pure C, compiled with plain cc.
#   make -C test/host dns
DNS_CFLAGS := -O1 -g -fsanitize=address,undefined -std=c11 -D_DEFAULT_SOURCE $(WARN) -I../../main -I.

dns: dns_tests
	./dns_tests

dns_tests: test_dns.c ../../main/dns_answer.c ../../main/dns_answer.h test.h
	$(CC) $(DNS_CFLAGS) -o $@ test_dns.c ../../main/dns_answer.c

dash_tests: $(SRCS) $(wildcard $(CORE)/include/*.h) $(wildcard $(CGM)/include/*.h) $(wildcard $(YPSO)/include/*.h) $(wildcard golden_*.h) host_io.h $(OUT)/libmbedcrypto.a
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS) $(OUT)/libmbedcrypto.a -lpthread -lm

$(OUT)/%.o: $(MBED)/library/%.c
	@mkdir -p $(OUT)
	$(CC) -O2 -I$(MBED)/include -I$(MBED)/library -c $< -o $@

$(OUT)/libmbedcrypto.a: $(MBED_OBJS)
	ar rcs $@ $^

clean:
	rm -f dash_tests medtrum_tests draw_tests serial_tests portal_tests dns_tests

.PHONY: test medtrum portal dns clean
