# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-2.0-only

obj-m := rpi4_amp.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
