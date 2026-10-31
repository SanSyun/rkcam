KERNELDIR := /root/rk3568/rk3568_linux_sdk/kernel
PWD := $(shell pwd)

ARCH ?= arm64
CROSS_COMPILE ?= aarch64-buildroot-linux-gnu-
TOOLCHAIN_DIR ?= /opt/atk-dlrk3568-5_10_sdk-toolchain/bin

obj-m := my_imx415.o

all:
	PATH=$(TOOLCHAIN_DIR):$$PATH $(MAKE) -C $(KERNELDIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) modules

clean:
	PATH=$(TOOLCHAIN_DIR):$$PATH $(MAKE) -C $(KERNELDIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) clean
