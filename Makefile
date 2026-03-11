# Quorum — Multi-Domain Agent Orchestration Daemon
# ============================================================

.PHONY: init build clean test help
.DEFAULT_GOAL := help

# ── Config ───────────────────────────────────────────────────

BUILD_DIR    := build
BUILD_TYPE   := Release
NPROC        := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
CONFIG       := configs/mm-bot.yaml

# ── Help ─────────────────────────────────────────────────────

help: ## Show this help
	@echo "Quorum — Multi-Domain Agent Orchestration"
	@echo ""
	@echo "Usage: make <target>"
	@echo ""
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

# ── Bootstrap ────────────────────────────────────────────────

init: ## Bootstrap project directory structure
	@echo "=== Bootstrapping Quorum project structure ==="

	@# ── quorum-core (C++, closed source) ──
	@mkdir -p quorum-core/src/daemon
	@mkdir -p quorum-core/src/agent
	@mkdir -p quorum-core/src/vault
	@mkdir -p quorum-core/src/storage
	@mkdir -p quorum-core/src/utils
	@mkdir -p quorum-core/src/knowledge
	@mkdir -p quorum-core/tests/unit
	@mkdir -p quorum-core/tests/integration

	@# ── Configs ──
	@mkdir -p configs/agents
	@mkdir -p configs/tasks

	@# ── Data directories (gitignored) ──
	@mkdir -p data/vaults
	@mkdir -p data/knowledge/inbox
	@mkdir -p data/knowledge/library
	@mkdir -p data/knowledge/archive

	@echo ""
	@echo "✓ Project structure created."
	@echo "  Next: run 'make build'"

# ── Build ────────────────────────────────────────────────────

build: ## Build C++ daemon and CLI
	@echo "=== Building quorum-core ==="
	cmake -B $(BUILD_DIR) -S quorum-core \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	cmake --build $(BUILD_DIR) -j$(NPROC)
	@echo "✓ Build complete: $(BUILD_DIR)/"

build-debug: ## Build with debug symbols
	$(MAKE) build BUILD_TYPE=Debug

# ── Run ──────────────────────────────────────────────────────

run: build ## Build and run daemon
	./$(BUILD_DIR)/quorum_daemon --config $(CONFIG)

run-verbose: build ## Build and run daemon with verbose logging
	./$(BUILD_DIR)/quorum_daemon --config $(CONFIG) --verbose

# ── Test ─────────────────────────────────────────────────────

test: build ## Run C++ tests
	cd $(BUILD_DIR) && ctest --output-on-failure

# ── Clean ────────────────────────────────────────────────────

clean: ## Remove build artifacts
	rm -rf $(BUILD_DIR)

clean-data: ## Remove runtime data (vaults, SQLite)
	rm -rf data/
	rm -f quorum.db quorum.db-wal quorum.db-shm

clean-all: clean clean-data ## Remove everything (build + data)

# ── Web ─────────────────────────────────────────────────────

web-install: ## Install web dashboard dependencies (server + client)
	cd quorum-web && bun install && cd client && bun install

web-dev: ## Start web API server (dev mode with watch, :3100)
	cd quorum-web && bun run dev

web-client: ## Start React frontend (dev mode, :3101 → proxy :3100)
	cd quorum-web && bun run dev:client

web-build: ## Build React frontend for production
	cd quorum-web && bun run build:client

web-start: ## Start web API server (production)
	cd quorum-web && bun run start

# ── Utilities ────────────────────────────────────────────────

fmt: ## Format C++ code (requires clang-format)
	find quorum-core/src -name '*.h' -o -name '*.cpp' | xargs clang-format -i

lint: ## Lint C++ code (requires clang-tidy)
	find quorum-core/src -name '*.h' -o -name '*.cpp' | \
		xargs clang-tidy -p $(BUILD_DIR)

loc: ## Count lines of code
	@echo "=== Lines of Code ==="
	@echo "C++:"
	@find quorum-core/src -name '*.h' -o -name '*.cpp' | xargs wc -l 2>/dev/null | tail -1 || echo "  0"
