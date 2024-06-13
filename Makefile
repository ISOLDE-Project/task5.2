ROOT_DIR := $(shell git rev-parse --show-toplevel)

num_cores         := $(shell nproc)
num_cores_half    := $(shell echo "$$(($(num_cores) / 2))")
num_cores_quarter := $(shell echo "$$(($(num_cores) / 4))")

INSTALL_PREFIX          ?= install
INSTALL_DIR             ?= ${ROOT_DIR}/${INSTALL_PREFIX}
LLVM_INSTALL_DIR        ?= ${INSTALL_DIR}/riscv-llvm
ONNX_INSTALL_DIR        ?= ${INSTALL_DIR}/onnx-mlir
PROTOC_INSTALL_DIR      ?= ${INSTALL_DIR}/protoc
CMAKE_INSTALL_DIR       ?= ${INSTALL_DIR}/cmake
MLIR_DIR                ?= ${ROOT_DIR}/toolchain/riscv-llvm/build/lib/cmake/mlir
PROTOC_DIR              ?= ${PROTOC_INSTALL_DIR}/bin

CC  := clang-10
CXX := clang++-10

CMAKE ?=  $(CMAKE_INSTALL_DIR)/bin/cmake




#all: toolchain-llvm-main

toolchain-onnx-mlir: Makefile
	git submodule update --init --recursive toolchain/onnx-mlir
	cd $(CURDIR)/toolchain/onnx-mlir && git reset --hard && git fetch && git checkout isolde/main && git submodule update --init --recursive
	mkdir -p $(ONNX_INSTALL_DIR)
	export PATH=$(PROTOC_DIR):$(PATH) && \
	cd $(ROOT_DIR)/toolchain/onnx-mlir && rm -rf build && mkdir -p build && cd build && \
	$(CMAKE)   \
	-DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
	-DONNX_MLIR_BUILD_TESTS=OFF \
	-DONNX_MLIR_ACCELERATORS=OFF \
	-DONNX_MLIR_ENABLE_STABLEHLO=OFF \
	-DCMAKE_C_COMPILER=$(CC) \
	-DCMAKE_CXX_COMPILER=$(CXX) \
	-DCMAKE_INSTALL_PREFIX=$(ONNX_INSTALL_DIR) \
	-DMLIR_DIR=${MLIR_DIR} \
	-DCMAKE_BUILD_TYPE="Debug" \
	..
	cd $(ROOT_DIR)/toolchain/onnx-mlir && \
	$(CMAKE) --build build --target install -j$(num_cores_half)

toolchain-cmake: toolchain/cmake-url 
	wget  `cat $(CURDIR)/$<` -O toolchain/cmake-linux-x86_64.tar.gz  
	mkdir -p $(CMAKE_INSTALL_DIR)
	cd toolchain && \
	tar -xzvf cmake-linux-x86_64.tar.gz  --strip-components=1 -C  $(CMAKE_INSTALL_DIR)

toolchain-llvm-main: Makefile
	mkdir -p $(LLVM_INSTALL_DIR)
	cd $(ROOT_DIR)/toolchain/riscv-llvm && rm -rf build && mkdir -p build && cd build && \
	$(CMAKE)   \
	-DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
	-DCMAKE_INSTALL_PREFIX=$(LLVM_INSTALL_DIR) \
	-DLLVM_ENABLE_PROJECTS="clang;lld;mlir" \
	-DCMAKE_BUILD_TYPE="Debug" \
	-DLLVM_ENABLE_ASSERTIONS=ON \
	-DLLVM_ENABLE_RTTI=ON \
	-DCMAKE_C_COMPILER=$(CC) \
	-DCMAKE_CXX_COMPILER=$(CXX) \
	-DLLVM_DEFAULT_TARGET_TRIPLE=riscv32-unknown-elf \
	-DLLVM_TARGETS_TO_BUILD="host;RISCV" \
	../llvm
	cd $(ROOT_DIR)/toolchain/riscv-llvm && \
	$(CMAKE) --build build --target install -j$(num_cores_quarter)

toolchain-protoc: Makefile	
	rm -fr $(PROTOC_INSTALL_DIR)
	git submodule update --init --recursive toolchain/protobuf
	cd toolchain/protobuf &&  git checkout v4.23.0
	mkdir -p $(PROTOC_INSTALL_DIR)
	cd $(ROOT_DIR)/toolchain/protobuf && rm -rf build && mkdir -p build && cd build && \
	$(CMAKE) \
    -DCMAKE_INSTALL_PREFIX=$(PROTOC_INSTALL_DIR) \
	-DCMAKE_CXX_STANDARD=14 \
	-Dprotobuf_BUILD_TESTS=OFF \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	..
	cd toolchain/protobuf && \
	$(CMAKE) --build build --target install -j$(num_cores_half)
	@echo "installed protoc: " `$(PROTOC_INSTALL_DIR)/bin/protoc --version`
	