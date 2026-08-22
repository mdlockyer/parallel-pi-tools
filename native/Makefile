CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -lcurl
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  LIB_EXT := dylib
  LIB_FLAGS := -dynamiclib
else
  LIB_EXT := so
  LIB_FLAGS := -shared
  LDFLAGS += -lpthread
endif

.PHONY: all clean

all: parallel libparallel.$(LIB_EXT)

# The CLI links the same core as the shared library so formatting and error
# handling can never drift between the two.
parallel: parallel.c cJSON.c cJSON.h parallel_lib.c
	$(CC) $(CFLAGS) -o $@ parallel.c parallel_lib.c cJSON.c $(LDFLAGS)

libparallel.$(LIB_EXT): parallel_lib.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -fPIC $(LIB_FLAGS) -o $@ parallel_lib.c cJSON.c $(LDFLAGS)

clean:
	rm -f parallel libparallel.dylib libparallel.so
