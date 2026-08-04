prj_path := $(shell git rev-parse --show-toplevel 2>/dev/null || pwd)

num_cores := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu)
num_cores_half := $(shell n=$$(($(num_cores)/2)); [ $$n -lt 1 ] && n=1; echo $$n)

VLT_CXX ?= clang++
VLT_CC  ?= clang

INSTALL_PREFIX             ?= eda
TOOLS_DIR                  ?= $(prj_path)/tools
INSTALL_DIR                ?= $(prj_path)/$(INSTALL_PREFIX)
SLANG_INSTALL_DIR          ?= ${INSTALL_DIR}/slang
SLANG_VERSION              ?= release/11.0
BENDER_INSTALL_DIR         ?= ${INSTALL_DIR}/bender
SLANG_NETLIST_INSTALL_DIR  ?= $(INSTALL_DIR)/slang-netlist
SLANG_NETLIST_VERSION      ?= v0.11.0



${TOOLS_DIR}:
	mkdir -p $@

${INSTALL_DIR}:
	mkdir -p $@

## Install slang
slang: ${TOOLS_DIR} ${INSTALL_DIR} ${SLANG_INSTALL_DIR}/bin/slang

${SLANG_INSTALL_DIR}/bin/slang: ${TOOLS_DIR} ${INSTALL_DIR}
	if [ ! -d "$(TOOLS_DIR)/slang/.git" ]; then \
		rm -rf "$(TOOLS_DIR)/slang"; \
		cd "$(TOOLS_DIR)" && git clone https://github.com/MikePopoloski/slang.git; \
	fi
	cd "$(TOOLS_DIR)/slang" && git fetch --all --tags
	cd "$(TOOLS_DIR)/slang" && test -z "$$(git status --porcelain)" || { echo "ERROR: $(TOOLS_DIR)/slang has local changes"; exit 1; }
	cd "$(TOOLS_DIR)/slang" && git checkout  "origin/$(SLANG_VERSION)"
	cd "$(TOOLS_DIR)/slang" && cmake -S . -B build \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$(SLANG_INSTALL_DIR)" \
		-DCMAKE_CXX_COMPILER="$(VLT_CXX)" \
		-DCMAKE_C_COMPILER="$(VLT_CC)"
	cd "$(TOOLS_DIR)/slang" && cmake --build build -j$(num_cores_half)
	cd "$(TOOLS_DIR)/slang" && cmake --install build


.PHONY: slang-netlist
slang-netlist: $(TOOLS_DIR) $(INSTALL_DIR) $(SLANG_NETLIST_INSTALL_DIR)/bin/slang-netlist

$(SLANG_NETLIST_INSTALL_DIR)/bin/slang-netlist: $(TOOLS_DIR) $(INSTALL_DIR)
	@if [ ! -d "$(TOOLS_DIR)/slang-netlist/.git" ]; then \
		rm -rf "$(TOOLS_DIR)/slang-netlist"; \
		cd "$(TOOLS_DIR)" && git clone https://github.com/jameshanlon/slang-netlist.git; \
	fi
	cd "$(TOOLS_DIR)/slang-netlist" && \
		git fetch --all --tags && \
		test -z "$$(git status --porcelain)" || { echo "ERROR: local changes"; exit 1; } && \
		git checkout "$(SLANG_NETLIST_VERSION)" && \
		git clean -fdx && \
		cmake -S . -B build \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_INSTALL_PREFIX="$(SLANG_NETLIST_INSTALL_DIR)" \
			-DENABLE_PY_BINDINGS=ON \
			-DCMAKE_CXX_COMPILER="$(VLT_CXX)" \
			-DCMAKE_C_COMPILER="$(VLT_CC)" && \
		cmake --build build -j$(num_cores_half) && \
		cmake --install build





