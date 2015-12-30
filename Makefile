CC	= gcc
CFLAGS	= -Wall -g -lpthread
PROG	= server
OBJS	= thread_pool.o

ALL: $(PROG) $(OBJS)

%.o: %.c %.h
	$(CC) -c $^ $(CFLAGS)
%: %.c $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS)

clean:
	$(RM) $(OBJS) $(PROG) $(wildcard *.h.gch) 
