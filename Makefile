# Quorum — deterministic Python tooling (spend readout; the vault index). No build.
.PHONY: help test web-test install uninstall doctor web

help: ## List targets
	@grep -E '^[a-z-]+:.*##' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-10s %s\n", $$1, $$2}'

test: ## Run every python test, then the web app's tests when it is installed
	@for t in scripts/*_test.py; do echo "== $$t"; python3 $$t || exit 1; done
	@if [ -d radio-web/node_modules ]; then $(MAKE) --no-print-directory web-test; else echo "== radio-web: not installed (quorum web build), skipped"; fi

web-test: ## Typecheck + unit-test Radio Studio's app
	@echo "== radio-web" && cd radio-web && npm run --silent typecheck && npm test --silent

web: ## Start Radio Studio (quorum web start)
	@python3 scripts/web_ctl.py start

install: ## Link the `quorum` CLI and the three skills; remove the retired daemon's links
	@scripts/install.sh

uninstall: ## Remove the links that point into this checkout
	@scripts/install.sh --uninstall

doctor: ## Read-only setup check
	@scripts/doctor.sh
