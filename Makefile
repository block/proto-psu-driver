# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
# Makefile for PSU PMBus Kernel Module

# Module name - will create psu-pmbus.ko from multiple source files
obj-m := psu-pmbus.o

# Path to libpsu portable library.
LIBPSU_DIR ?= lib

# List all source files to compile and link into the module
# Kbuild will automatically compile .c files to .o files
psu-pmbus-y := psu_pmbus.o \
               psu_pmbus_platform.o \
               psu_pmbus_protocol.o \
               psu_hwmon.o \
               psu_sysfs.o \
               psu_trace.o \
               chicony/psu_chicony_s24.o \
               boco/psu_boco_common.o \
               boco/psu_boco_bs402a17.o \
               boco/psu_boco_bs502a17.o \
               $(LIBPSU_DIR)/src/pmbus_conversion.o

# Driver version - derived from git for local builds. Out-of-tree builds
# without a .git tree (e.g. Yocto recipes) should pass PSU_PMBUS_VERSION
# explicitly, e.g. EXTRA_OEMAKE += "PSU_PMBUS_VERSION=${SRCREV}".
#
# $(src) is set by kbuild to the module source dir during the M= pass;
# $(CURDIR) is correct for direct `make` from the module dir.
PSU_PMBUS_VERSION ?= $(shell git -C $(if $(src),$(src),$(CURDIR)) describe --always --dirty 2>/dev/null || echo "unknown")

# Add source directory to include path so subdirectories can use #include "psu_pmbus.h"
# Add libpsu include path for portable PMBus protocol definitions
ccflags-y += -I$(src) -I$(src)/$(LIBPSU_DIR)/include
ccflags-y += -DPSU_PMBUS_VERSION='"$(PSU_PMBUS_VERSION)"'

# Kernel source directory
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

# Build targets
all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
	rm -f *.o *~ core .depend .*.cmd *.ko *.mod.c *.mod
	rm -f Module.markers Module.symvers modules.order
	rm -rf .tmp_versions Modules.symvers

help:
	@echo "PSU PMBus Kernel Module Build"
	@echo ""
	@echo "Targets:"
	@echo "  all             - Build the kernel module"
	@echo "  modules_install - Install the module to system"
	@echo "  clean           - Remove build artifacts"
	@echo "  help            - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  KERNEL_SRC      - Path to kernel source (default: /lib/modules/\$(uname -r)/build)"
	@echo "  LIBPSU_DIR      - Path to libpsu sources (default: lib)"

.PHONY: all modules_install clean help
