
CLEAN_TARGETS := $(addprefix clean-,$(CLEAN_TOOLS))

.PHONY: clean $(CLEAN_TARGETS)

clean: $(CLEAN_TARGETS)

$(CLEAN_TARGETS): clean-%:
	cd $(TOOLS_DIR)/$* && git clean -xfdf
	
check-conda-ibex:
	@if [ "$$CONDA_DEFAULT_ENV" != "ibex" ]; then \
		echo "Error: Conda environment 'ibex' must be active."; \
		echo "Run: source ./eth.sh"; \
		exit 1; \
	fi

help: Makefile
	@printf "Available targets:\n------------------\n"
	@for mkfile in $(MAKEFILE_LIST); do \
		awk '/^[a-zA-Z\-_0-9]+:/ { \
			helpMessage = match(lastLine, /^## (.*)/); \
			if (helpMessage) { \
				helpCommand = substr($$1, 0, index($$1, ":")-1); \
				helpMessage = substr(lastLine, RSTART + 3, RLENGTH); \
				printf "%-20s %s\n", helpCommand, helpMessage; \
			} \
		} \
		{ lastLine = $$0 }' $$mkfile; \
	done

.PHONY: help