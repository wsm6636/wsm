# wsm

## litmus-rt

cp ./*.c LITMUS-RT_ROOT_DIR/litmus.

cp ./*.h LTMUS-RT_ROOT_DIR/include/litmus.

re-compile and re-install the kernel

## memguard

# install the modules

make
sudo insmod memguarddemo.ko

# uninstall the modules

sudo rmmod memguarddemo.ko

# log

bash test.sh


## memtest
memguard modules test
download

git clone https://github.com/yuhcaesar/memtest.git

# install

make pnd
bash cat.sh

# use
assign 900,100,100,100 MB/s for Core 0,1,2,3

sudo su
echo mb 900 100 100 100 > /sys/kernel/debug/memguard/limit


## parsec
real-time task and task set

## result



