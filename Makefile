CFLAGS+=-I/usr/local/include
LDFLAGS+=-L/usr/local/lib -lrt -lzmq 

all: rtifssd
rtifssd: main.c
	cc main.c -o rtifssd $(CFLAGS) $(LDFLAGS)

clean:
	@rm a.out
