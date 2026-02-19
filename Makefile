# noclaw — The absolute smallest AI assistant. Pure C.
# Target: <100KB binary, <500KB RAM, <1ms startup.

CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter
LDFLAGS :=

# Source files
SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)

# Output
BIN := noclaw

# ── Platform detection ───────────────────────────────────────────

UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
  # macOS: SecureTransport for TLS (system framework, no extra deps)
  LDFLAGS += -framework Security -framework CoreFoundation
else
  # Linux: BearSSL for TLS (tiny footprint, ~200KB RSS vs OpenSSL's ~4.6MB)
  LDFLAGS += -lbearssl
endif

# ── Build modes ──────────────────────────────────────────────────

.PHONY: all release debug clean test install uninstall

all: release

release: CFLAGS  += -Os -DNDEBUG -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables
ifeq ($(UNAME),Darwin)
release: LDFLAGS += -flto -Wl,-dead_strip
else
release: LDFLAGS += -flto -Wl,--gc-sections
endif
release: $(BIN)
	@ls -lh $(BIN) | awk '{print "Binary: " $$5}'

debug: CFLAGS += -O0 -g -DDEBUG -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(BIN)

# ── Link ─────────────────────────────────────────────────────────

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Compile ──────────────────────────────────────────────────────

src/%.o: src/%.c src/nc.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Test ─────────────────────────────────────────────────────────

test: CFLAGS += -O0 -g -DDEBUG -DNC_TEST
test:
	$(CC) $(CFLAGS) -o noclaw_test src/*.c -DNC_TEST_MAIN $(LDFLAGS)
	./noclaw_test
	@rm -f noclaw_test

# ── Musl static build (Linux only) ───────────────────────────

.PHONY: musl
musl: CC := musl-gcc
musl: CFLAGS  += -Os -DNDEBUG -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -Iinc
musl: LDFLAGS := -static lib/libbearssl.a -lm -flto -Wl,--gc-sections
musl: $(BIN)
	@strip -s $(BIN)
	@ls -lh $(BIN) | awk '{print "Binary: " $$5 " (static, stripped)"}'

# ── Install ──────────────────────────────────────────────────────

PREFIX ?= /usr/local

install: release
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)

# ── Clean ────────────────────────────────────────────────────────

clean:
	rm -f src/*.o $(BIN) noclaw_test

# ── Size report ──────────────────────────────────────────────────

.PHONY: size
size: release
	@echo "--- Binary size ---"
	@ls -lh $(BIN)
	@echo "--- Section sizes ---"
	@size $(BIN) 2>/dev/null || true
