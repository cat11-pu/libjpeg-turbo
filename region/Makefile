# Standalone build for the narrow region-decode interface and its test.
# Usage: make BUILD=../build (BUILD points at a configured libjpeg-turbo build).
BUILD ?= ../build

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Werror -std=c99
CPPFLAGS += -I.. -I$(BUILD)
LDLIBS  += $(BUILD)/libjpeg.a -lm

OBJS = regiondec.o rdec_test.o rdec.o

all: rdec_test rdec

rdec_test: regiondec.o rdec_test.o
	$(CC) $(CFLAGS) -o $@ regiondec.o rdec_test.o $(LDLIBS)

rdec: regiondec.o rdec.o
	$(CC) $(CFLAGS) -o $@ regiondec.o rdec.o $(LDLIBS)

%.o: %.c regiondec.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) rdec_test rdec

.PHONY: all clean
