PI_EXTENSIONS_DIR := $(HOME)/.pi/agent/extensions
EXTENSION := parallel-search.ts

.PHONY: install uninstall reinstall test bench all

install:
	@mkdir -p "$(PI_EXTENSIONS_DIR)"
	@cp "$(EXTENSION)" "$(PI_EXTENSIONS_DIR)/$(EXTENSION)"
	@cp -r lib "$(PI_EXTENSIONS_DIR)/"
	@echo "✓ Installed $(EXTENSION) + lib/ to $(PI_EXTENSIONS_DIR)"
	@echo "  Run /reload in Pi to activate"

uninstall:
	@rm -f "$(PI_EXTENSIONS_DIR)/$(EXTENSION)"
	@rm -rf "$(PI_EXTENSIONS_DIR)/lib"
	@echo "✓ Removed $(EXTENSION) + lib/ from $(PI_EXTENSIONS_DIR)"

reinstall: uninstall install

test:
	@node --test test/unit.mjs

bench:
	@node test/benchmark.mjs

all: test bench
