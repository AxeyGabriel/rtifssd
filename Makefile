CFLAGS+=-I/usr/local/include
LDFLAGS+=-L/usr/local/lib -lrt -lzmq 

all: a.out
a.out: main.c
	cc main.c $(CFLAGS) $(LDFLAGS)

clean:
	@rm a.out
