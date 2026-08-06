CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -lcurl

.PHONY: all clean

all: parallel

parallel: parallel.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o $@ parallel.c cJSON.c $(LDFLAGS)

clean:
	rm -f parallel
