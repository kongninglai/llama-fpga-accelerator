ifneq ($(KERNELRELEASE),)
obj-m += tiled_gemv_drv.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
CROSS_COMPILE ?=
APP_CC ?= $(CROSS_COMPILE)gcc
HOST_CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror -ffp-contract=off
LDLIBS = -lm
COMMON = tg_runtime.c tg_backend.c q8.c
HEADERS = tg_runtime.h tg_backend.h q8.h tiled_gemv_abi.h ../../hls/tiled-gemv/tiled_gemv_regs.h

.PHONY: all app module test sanitize ubsan clean overlay
all: app
app: build/tiled_gemv_test
build:
	mkdir -p $@
build/tiled_gemv_test: tiled_gemv_test.c $(COMMON) $(HEADERS) | build
	$(APP_CC) $(CFLAGS) tiled_gemv_test.c $(COMMON) $(LDLIBS) -o $@
build/test_runtime: test_runtime.c $(COMMON) $(HEADERS) | build
	$(HOST_CC) $(CFLAGS) test_runtime.c $(COMMON) $(LDLIBS) -o $@
test: build/test_runtime
	./build/test_runtime
sanitize: | build
	$(HOST_CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_runtime.c $(COMMON) $(LDLIBS) -o build/test_runtime_sanitize
	./build/test_runtime_sanitize
ubsan: | build
	$(HOST_CC) $(CFLAGS) -O1 -fsanitize=undefined -fno-sanitize-recover=all test_runtime.c $(COMMON) $(LDLIBS) -o build/test_runtime_ubsan
	./build/test_runtime_ubsan
module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
overlay: build/tiled-gemv.dtbo
build/tiled-gemv.dtbo: tiled-gemv.dtso | build
	dtc -@ -I dts -O dtb -o $@ $<
clean:
	$(RM) build/tiled_gemv_test build/test_runtime build/test_runtime_sanitize build/test_runtime_ubsan build/tiled-gemv.dtbo
	$(RM) *.o *.ko *.mod *.mod.c Module.symvers modules.order .*.cmd
endif
