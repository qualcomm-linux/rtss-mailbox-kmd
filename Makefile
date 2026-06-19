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

headers_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) headers_install $(KBUILD_OPTIONS)

