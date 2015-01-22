CFLAGS = -O3 -g -Wall -march=native -Werror -fPIC -fstack-protector --param=ssp-buffer-size=4 -Wformat -Werror=format-security
LIBS = -lm
LDFLAGS = -Wl,--gc-sections -Wl,-z,relro,-PIE -fPIC

#objs = mon.o

all : mon

mon : mon.c $(objs)
	$(CC) $(CFLAGS) $(LDFLAGS) -o mon mon.c $(objs) $(LIBS)
	sudo chown root.root mon 
	sudo chmod u+s mon
