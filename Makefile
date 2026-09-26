trigger2-y := \
	trigger2_connector.o \
	trigger2_drm.o \
	trigger2_drv.o \
	trigger2_registers.o \
	trigger2_transfer.o

obj-m := trigger2.o

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

all:	modules

modules:
	make CHECK="/usr/bin/sparse" -C $(KSRC) M=$(PWD) modules

clean:
	make -C $(KSRC) M=$(PWD) clean
	rm -f $(PWD)/Module.symvers $(PWD)/*.ur-safe
