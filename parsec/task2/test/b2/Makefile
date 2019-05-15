-include make.conf
LIBLITMUS ?= /home/wsm/litmus/liblitmus
include ${LIBLITMUS}/inc/config.makefile
CPPFLAGS += -Iinclude/
#CFLAGS +=-D_PERIOD=100 -D_DEADLINE=100 -D_EXEC_COST=10
all=blackscholes2
.PHONY:all clean  
all:${all}  
clean:
	rm -f ${all} *.o *.d
obj-blackscholes2=blackscholes2.o
blackscholes2: ${obj-blackscholes2} -lm  

include ${LIBLITMUS}/inc/depend.makefile
