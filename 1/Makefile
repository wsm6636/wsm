-include make.conf
LIBLITMUS ?= /home/wsm/litmus-mem/liblitmus
include ${LIBLITMUS}/inc/config.makefile
CPPFLAGS += -Iinclude/
#CFLAGS +=-D_PERIOD=100 -D_DEADLINE=100 -D_EXEC_COST=10
all=rt-task
.PHONY:all clean  
all:${all}  
clean:
	rm -f ${all} *.o *.d
obj-rt-task=rt-task.o
rt-task: ${obj-rt-tasl} -lm  

include ${LIBLITMUS}/inc/depend.makefile
