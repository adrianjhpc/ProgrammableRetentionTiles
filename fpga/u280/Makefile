VPP ?= v++
TARGET ?= hw
PLATFORM ?=
BUILD_DIR ?= build
KERNEL := $(BUILD_DIR)/rtmem_migrate.xo
XCLBIN := $(BUILD_DIR)/rtmem_u280.$(TARGET).xclbin

.PHONY: all check-platform clean

all: $(XCLBIN)

check-platform:
	@test -n "$(PLATFORM)" || \
		(echo "Set PLATFORM to the installed U280 platform .xpfm"; exit 1)

$(KERNEL): rtmem_migrate.cpp | check-platform
	mkdir -p $(BUILD_DIR)
	$(VPP) -c -t $(TARGET) --platform $(PLATFORM) \
		-k rtmem_migrate $< -o $@

$(XCLBIN): $(KERNEL) connectivity.cfg | check-platform
	$(VPP) -l -t $(TARGET) --platform $(PLATFORM) \
		--config connectivity.cfg $(KERNEL) -o $@

clean:
	rm -rf $(BUILD_DIR)
