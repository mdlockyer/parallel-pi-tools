PI_EXTENSIONS_DIR := $(HOME)/.pi/agent/extensions
EXTENSION := parallel-search.ts

.PHONY: install uninstall reinstall

install:
	@mkdir -p "$(PI_EXTENSIONS_DIR)"
	@cp "$(EXTENSION)" "$(PI_EXTENSIONS_DIR)/$(EXTENSION)"
	@echo "✓ Installed $(EXTENSION) to $(PI_EXTENSIONS_DIR)"
	@echo "  Run /reload in Pi to activate"

uninstall:
	@rm -f "$(PI_EXTENSIONS_DIR)/$(EXTENSION)"
	@echo "✓ Removed $(EXTENSION) from $(PI_EXTENSIONS_DIR)"

reinstall: uninstall install
