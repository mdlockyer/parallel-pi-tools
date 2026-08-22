PI_EXTENSIONS_DIR := $(HOME)/.pi/agent/extensions
EXTENSION := web-search.ts
LIB_DIR := lib/web-search
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  LIB_FILE := native/libparallel.dylib
  VERIFY_LIBRARY := codesign --verify --verbose=2
else
  LIB_FILE := native/libparallel.so
  VERIFY_LIBRARY := true
endif

.PHONY: install uninstall reinstall test bench all native native-clean

install: native
	@mkdir -p "$(PI_EXTENSIONS_DIR)"
	@cp "$(EXTENSION)" "$(PI_EXTENSIONS_DIR)/$(EXTENSION)"
	@rm -rf "$(PI_EXTENSIONS_DIR)/$(LIB_DIR)"
	@mkdir -p "$(PI_EXTENSIONS_DIR)/$(LIB_DIR)"
	@cp $(LIB_DIR)/ffi.mjs $(LIB_DIR)/parallel.mjs "$(PI_EXTENSIONS_DIR)/$(LIB_DIR)/"
	@set -e; \
		tmp="$$(mktemp "$(PI_EXTENSIONS_DIR)/$(LIB_DIR)/.$(notdir $(LIB_FILE)).XXXXXX")"; \
		trap 'rm -f "$$tmp"' EXIT; \
		cp -p "$(LIB_FILE)" "$$tmp"; \
		$(VERIFY_LIBRARY) "$$tmp"; \
		mv -f "$$tmp" "$(PI_EXTENSIONS_DIR)/$(LIB_DIR)/$(notdir $(LIB_FILE))"; \
		trap - EXIT
	@if [ ! -d "$(PI_EXTENSIONS_DIR)/node_modules/koffi" ]; then \
		echo "Installing koffi for extensions (one-time)..."; \
		cd "$(PI_EXTENSIONS_DIR)" && npm install --no-save --no-audit --no-fund koffi@^3.1.4 >/dev/null 2>&1 \
			|| echo "  (koffi install failed - extension will use the JS path)"; \
	fi
	@echo "✓ Installed $(EXTENSION) + $(LIB_DIR)/ (incl. a copy of $(notdir $(LIB_FILE))) to $(PI_EXTENSIONS_DIR)"
	@echo "  Run /reload in Pi to activate"

uninstall:
	@rm -f "$(PI_EXTENSIONS_DIR)/$(EXTENSION)"
	@rm -rf "$(PI_EXTENSIONS_DIR)/$(LIB_DIR)"
	@echo "✓ Removed $(EXTENSION) + $(LIB_DIR) from $(PI_EXTENSIONS_DIR)"

reinstall: uninstall install

native:
	@$(MAKE) -C native

native-clean:
	@$(MAKE) -C native clean

test: native
	@node --test test/unit.mjs test/install.mjs

bench: native
	@node test/benchmark.mjs

all: test bench
