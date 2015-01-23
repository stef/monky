CFLAGS = -g -Wall -march=native -Werror -fPIC -fstack-protector --param=ssp-buffer-size=4 -Wformat -Werror=format-security
LIBS = -lseccomp
LDFLAGS = -Wl,--gc-sections -Wl,-z,relro,-PIE -fPIC

#objs = monky.o

all : monky

monky : monky.c $(objs)
	$(CC) $(CFLAGS) $(LDFLAGS) -o monky monky.c $(objs) $(LIBS)
	sudo chown root.root monky
	sudo chmod u+s monky
