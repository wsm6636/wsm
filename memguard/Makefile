obj-m=memguarddemo.o

KVERSION=$(shell uname -r)
BLDDIR=/lib/modules/$(KVERSION)/build

all:
	make -C $(BLDDIR) M=$(PWD) modules

clean:
	make -C $(BLDDIR) M=$(PWD) clean
