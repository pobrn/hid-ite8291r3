# SPDX-License-Identifier: GPL-2.0
MODNAME = hid-ite8291r3
MODVER = 0.0

obj-m += $(MODNAME).o

KDIR = /lib/modules/$(shell uname -r)/build
MDIR = /usr/src/$(MODNAME)-$(MODVER)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

dkmsinstall:
	mkdir -p $(MDIR)
	cp Makefile dkms.conf $(wildcard *.c) $(MDIR)/.
	dkms add $(MODNAME)/$(MODVER)
	dkms build $(MODNAME)/$(MODVER)
	dkms install $(MODNAME)/$(MODVER)

dkmsuninstall:
	-rmmod $(MODNAME)
	-dkms uninstall $(MODNAME)/$(MODVER)
	-dkms remove $(MODNAME)/$(MODVER) --all
	rm -rf $(MDIR)
