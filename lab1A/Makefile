CC=gcc
C_FLAGS=-Wall -fsanitize=address,undefined

sample1 : lab1.c
	${CC} ${C_FLAGS} -o lab1.out lab1.c

.PHONY: clean
clean:
	rm *.out