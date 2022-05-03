CC=gcc
C_FLAGS=-Wall -fsanitize=address,undefined

lab3 : lab3.c
	${CC} ${C_FLAGS} -o lab3.out lab3.c

.PHONY: clean
clean:
	rm *.out