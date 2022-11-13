CFLAGS=-Wall

all : test

test : test.o mtmq.o
	gcc $^ $(LIBS) -o $@

%.o : %.c
	gcc $(CFLAGS) -c $< -o $@

.PHONY : clean
clean :
	rm -f *.o test
