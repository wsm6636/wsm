#!/bin/bash
#	 sudo ./flu/fluidanimate-rt/fluidanimate 1 1 ~/parsec-rt/fluidanimate-rt/input/in_small.fluid ./out/out3.fluid &
	sudo ./b/blackscholes 1 ~/parsec-rt/blackscholes-rt/input/in_small.txt ./out/out1.txt &
#	./test &
	sudo ./dedup/dedup-rt/dedup -c -p -v -t 1 -i ~/parsec-rt/dedup-rt/input/in_test.dat -o ./out/out2.dat.ddp &
#	 sudo ./b/blackscholes 1 ~/parsec-rt/blackscholes-rt/input/in_small.txt ./out/out1.txt &

	
#	./test &
#	sudo ./flu/fluidanimate-rt/fluidanimate 1 1 ~/parsec-rt/fluidanimate-rt/input/in_small.fluid ./out/out3.fluid &
#	./test &
 	sudo ./stream/streamcluster-rt/streamcluster 3 10 3 16 16 10 none ./out/out4.txt 1 &
#	sudo ./swap/swaptions -ns 16 -sm 10000 -nt 1 &
wait



