VPP ?= v++
TARGET ?= hw
PLATFORM ?=
BUILD_DIR ?= build
MIGRATION_KERNEL := $(BUILD_DIR)/rtmem_migrate.xo
MATMUL_KERNEL := $(BUILD_DIR)/rtmem_blocked_matmul.xo
KERNELS := $(MIGRATION_KERNEL) $(MATMUL_KERNEL)
XCLBIN := $(BUILD_DIR)/rtmem_u280.$(TARGET).xclbin

.PHONY: all check-platform clean

all: $(XCLBIN)

check-platform:
	@test -n "$(PLATFORM)" || \
		(echo "Set PLATFORM to the installed U280 platform .xpfm"; exit 1)

$(MIGRATION_KERNEL): rtmem_migrate.cpp | check-platform
	mkdir -p $(BUILD_DIR)
	$(VPP) -c -t $(TARGET) --platform $(PLATFORM) \
		-k rtmem_migrate $< -o $@

$(MATMUL_KERNEL): rtmem_blocked_matmul.cpp | check-platform
	mkdir -p $(BUILD_DIR)
	$(VPP) -c -t $(TARGET) --platform $(PLATFORM) \
		-k rtmem_blocked_matmul $< -o $@

$(XCLBIN): $(KERNELS) connectivity.cfg | check-platform
	$(VPP) -l -t $(TARGET) --platform $(PLATFORM) \
		--config connectivity.cfg $(KERNELS) -o $@

clean:
	rm -rf $(BUILD_DIR)
