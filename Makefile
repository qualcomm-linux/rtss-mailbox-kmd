# SPDX-License-Identifier: GPL-2.0-only

BT_ROOT = $(shell pwd)
all: modules

KBUILD_OPTIONS += CONFIG_QCOM_RTSS_MAILBOX=m
KBUILD_OPTIONS += -Wall -Wmissing-prototypes

$(info KBUILD_OPTIONS: $(KBUILD_OPTIONS))
$(info KERNEL_SRC: $(KERNEL_SRC))

SRC := $(shell pwd)

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) $(KBUILD_OPTIONS) clean

# headers_install is a top-level kernel Makefile target that exports the whole
# kernel's UAPI headers; it is not reachable through an out-of-tree module's
# M= build path, so routing it through $(KERNEL_SRC) as the other targets do
# does not work. Install the UAPI header directly instead.
INSTALL_HDR_PATH ?= usr

headers_install:
	install -d $(INSTALL_HDR_PATH)/include
	install -m 0644 include/uapi/rtss_mailbox_uapi.h $(INSTALL_HDR_PATH)/include/

