

XLEN?=32
ONNX_MODEL              ?= ${ROOT_DIR}/apps/ResNet-18/models/onnx/resnet18.onnx
APP=resnet18

all: graph.o.dump


include ../../common/common.mk


graph.o.dump: graph
	$(RISCV_OBJDUMP) $(RISCV_OBJDUMP_FLAGS) -x $<.o > $@




