
prj_path := $(shell git rev-parse --show-toplevel)
num_cores := $(shell nproc)
num_cores_half := $(shell echo "$$(($(num_cores) / 2)"))
VLT_CXX := /usr/bin/clang++
VLT_CC  := /usr/bin/clang


INSTALL_PREFIX            ?= install
TOOLS_DIR                 ?= $(prj_path)/tools
INSTALL_DIR               ?= $(prj_path)/${INSTALL_PREFIX}
SLANG_INSTALL_DIR         ?= ${INSTALL_DIR}/slang
SLANG_VERSION             ?= release/11.0
BENDER_INSTALL_DIR        ?= ${INSTALL_DIR}/bender



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







