# wsm

# litmus-rt

#cp ./*.c LITMUS-RT_ROOT_DIR/litmus.

#cp ./*.h LTMUS-RT_ROOT_DIR/include/litmus.

re-compile and re-install the kernel


# memguard

## memguard driver

#cp ./memguard/ KERNEL_ROOT_DIR/drivers

Append file KERNEL_ROOT_DIR/drivers/Kconfig.

#source "drivers/memguard/Kconfig"

Append file KERNEL_ROOT_DIR/drivers/Makefile.

#obj-$(CONFIG_MEMGUARD) += memguard/

check kernel settings are as follows:

#MEMGUARD=Y

re-compile and re-install the kernel.
## use

assign 900 MB/s for Core 0

#sudo su

#echo 0 900 > /sys/kernel/debug/memguard/limit

assign 200 MB/s for Core 1

#sudo su

#echo 1 200 > /sys/kernel/debug/memguard/limit

## log

#bash test.sh


# memtest

memguard modules test

## download

#git clone https://github.com/yuhcaesar/memtest.git

## install

#make pnd

#bash cat.sh


# parsec

real-time task and task set

# result




