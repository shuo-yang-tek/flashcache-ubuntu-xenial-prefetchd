all:
	gcc -o reader.o reader.c
	gcc -o step_reader.o step_reader.c
	gcc -o interleave-reader.o interleave-reader.c -lrt -pthread

clean:
	rm reader.o
	rm interleave-reader.o
