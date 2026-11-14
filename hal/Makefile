obj-m += fpga_drv.o

KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
APP_CC ?= $(CROSS_COMPILE)gcc
APP_CFLAGS ?= -O2 -Wall -Wextra
APP_LDFLAGS ?=

.PHONY: all app clean test
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

app: dotProd_app

dotProd_app: dotProd_app.c
	$(APP_CC) $(APP_CFLAGS) $< $(APP_LDFLAGS) -o $@

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

test:
	python3 test_driver.py
