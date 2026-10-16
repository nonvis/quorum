# Quorum — deterministic Python tooling (spend readout; the vault index). No build.
.PHONY: help test install uninstall doctor

help: ## List targets
	@grep -E '^[a-z-]+:.*##' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-10s %s\n", $$1, $$2}'

test: ## Run every python test
	@for t in scripts/*_test.py; do echo "== $$t"; python3 $$t || exit 1; done

install: ## Link the `quorum` CLI and the three skills; remove the retired daemon's links
	@scripts/install.sh

uninstall: ## Remove the links that point into this checkout
	@scripts/install.sh --uninstall

doctor: ## Read-only setup check
	@scripts/doctor.sh
