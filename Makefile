# Quorum — Multi-Domain Agent Orchestration Daemon
# ============================================================

.PHONY: init build clean test deploy-contracts help
.DEFAULT_GOAL := help

# ── Config ───────────────────────────────────────────────────

BUILD_DIR    := build
BUILD_TYPE   := Release
NPROC        := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
CONFIG       := configs/mm-bot.yaml
SUI_NETWORK  := testnet

# ── Help ─────────────────────────────────────────────────────

help: ## Show this help
	@echo "Quorum — Multi-Domain Agent Orchestration"
	@echo ""
	@echo "Usage: make <target>"
	@echo ""
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

# ── Bootstrap ────────────────────────────────────────────────

init: ## Bootstrap full project directory structure
	@echo "=== Bootstrapping Quorum project structure ==="

	@# ── quorum-core (C++, closed source) ──
	@mkdir -p quorum-core/src/daemon
	@mkdir -p quorum-core/src/agent
	@mkdir -p quorum-core/src/vault
	@mkdir -p quorum-core/src/chain
	@mkdir -p quorum-core/src/seal
	@mkdir -p quorum-core/src/storage
	@mkdir -p quorum-core/src/utils
	@mkdir -p quorum-core/src/sdk
	@mkdir -p quorum-core/src/cli/commands
	@mkdir -p quorum-core/tests/unit
	@mkdir -p quorum-core/tests/integration
	@mkdir -p quorum-core/tests/e2e

	@# ── quorum-contracts (Move, open source) ──
	@mkdir -p quorum-contracts/sources
	@mkdir -p quorum-contracts/tests

	@# ── quorum-ts (TypeScript, open source) ──
	@mkdir -p quorum-ts/packages/sdk/src
	@mkdir -p quorum-ts/packages/cli/src/commands
	@mkdir -p quorum-ts/packages/dashboard/src/pages
	@mkdir -p quorum-ts/packages/dashboard/src/components

	@# ── quorum-docs ──
	@mkdir -p quorum-docs/docs

	@# ── Configs ──
	@mkdir -p configs/agents
	@mkdir -p configs/tasks

	@# ── Data directories (gitignored) ──
	@mkdir -p data/vaults/market_analyst/knowledge
	@mkdir -p data/vaults/market_analyst/experiments
	@mkdir -p data/vaults/market_analyst/decisions
	@mkdir -p data/vaults/market_analyst/inbox
	@mkdir -p data/vaults/bot_analyst/knowledge
	@mkdir -p data/vaults/bot_analyst/experiments
	@mkdir -p data/vaults/bot_analyst/decisions
	@mkdir -p data/vaults/bot_analyst/inbox
	@mkdir -p data/vaults/engineer/knowledge
	@mkdir -p data/vaults/engineer/experiments
	@mkdir -p data/vaults/engineer/decisions
	@mkdir -p data/vaults/engineer/inbox
	@mkdir -p data/vaults/operator/knowledge
	@mkdir -p data/vaults/operator/experiments
	@mkdir -p data/vaults/operator/decisions
	@mkdir -p data/vaults/operator/inbox

	@# ── Generate stub files (won't overwrite existing) ──
	@test -f quorum-core/CMakeLists.txt || cp scaffolds/CMakeLists.txt quorum-core/CMakeLists.txt 2>/dev/null || true
	@test -f quorum-core/src/main.cpp || touch quorum-core/src/main.cpp
	@test -f quorum-contracts/Move.toml || cp scaffolds/Move.toml quorum-contracts/Move.toml 2>/dev/null || true

	@# ── Touch header stubs ──
	@for f in scheduler router consensus event_dispatcher message_bus; do \
		test -f quorum-core/src/daemon/$$f.h || touch quorum-core/src/daemon/$$f.h; \
	done
	@for f in invoker context_assembler output_parser model_router; do \
		test -f quorum-core/src/agent/$$f.h || touch quorum-core/src/agent/$$f.h; \
	done
	@for f in vault_manager walrus_client retention indexer; do \
		test -f quorum-core/src/vault/$$f.h || touch quorum-core/src/vault/$$f.h; \
	done
	@for f in sui_client proposal agent_identity audit ptb_builder; do \
		test -f quorum-core/src/chain/$$f.h || touch quorum-core/src/chain/$$f.h; \
	done
	@for f in seal_client policy cross_vault; do \
		test -f quorum-core/src/seal/$$f.h || touch quorum-core/src/seal/$$f.h; \
	done
	@for f in database local_cache; do \
		test -f quorum-core/src/storage/$$f.h || touch quorum-core/src/storage/$$f.h; \
	done
	@for f in http_client json crypto config; do \
		test -f quorum-core/src/utils/$$f.h || touch quorum-core/src/utils/$$f.h; \
	done
	@for f in quorum agent_builder task_builder; do \
		test -f quorum-core/src/sdk/$$f.h || touch quorum-core/src/sdk/$$f.h; \
	done
	@test -f quorum-core/src/cli/cli_main.cpp || touch quorum-core/src/cli/cli_main.cpp
	@for f in init agent proposal vault daemon_cmd audit; do \
		test -f quorum-core/src/cli/commands/$$f.cpp || touch quorum-core/src/cli/commands/$$f.cpp; \
	done

	@# ── Touch Move contract stubs ──
	@for f in proposal agent audit vault_access; do \
		test -f quorum-contracts/sources/$$f.move || touch quorum-contracts/sources/$$f.move; \
	done

	@echo ""
	@echo "✓ Project structure created."
	@echo "  Next: populate src files and run 'make build'"

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

test-contracts: ## Run Move contract tests
	cd quorum-contracts && sui move test

test-ts: ## Run TypeScript tests
	cd quorum-ts && pnpm test

test-all: test test-contracts test-ts ## Run all tests

# ── Move Contracts ───────────────────────────────────────────

build-contracts: ## Build Move contracts
	cd quorum-contracts && sui move build

deploy-contracts: build-contracts ## Deploy Move contracts to Sui testnet
	cd quorum-contracts && sui client publish --gas-budget 100000000

# ── TypeScript ───────────────────────────────────────────────

ts-install: ## Install TypeScript dependencies
	cd quorum-ts && pnpm install

ts-build: ts-install ## Build TypeScript packages
	cd quorum-ts && pnpm build

ts-dev: ts-install ## Start dashboard in dev mode
	cd quorum-ts && pnpm --filter @quorum/dashboard dev

# ── Clean ────────────────────────────────────────────────────

clean: ## Remove build artifacts
	rm -rf $(BUILD_DIR)
	rm -rf quorum-contracts/build
	rm -rf quorum-ts/node_modules quorum-ts/packages/*/dist

clean-data: ## Remove runtime data (vaults, SQLite)
	rm -rf data/
	rm -f quorum.db quorum.db-wal quorum.db-shm

clean-all: clean clean-data ## Remove everything (build + data)

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
	@echo "Move:"
	@find quorum-contracts/sources -name '*.move' | xargs wc -l 2>/dev/null | tail -1 || echo "  0"
	@echo "TypeScript:"
	@find quorum-ts/packages -name '*.ts' -o -name '*.tsx' | xargs wc -l 2>/dev/null | tail -1 || echo "  0"
