PORT = 52061
FLAGS = -DPORT=$(PORT) -Wall -g -std=gnu99

wordsrv : wordsrv.o socket.o gameplay.o
	gcc $(FLAGS) -o $@ $^

%.o : %.c socket.h gameplay.h
	gcc $(FLAGS) -c $<

clean :
	rm *.o wordsrv
