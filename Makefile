INCLUDES =  -I/usr/include/freetype2

# for debugging
#CFLAGS = -g -Wall -march=native -Werror -fPIC -fstack-protector --param=ssp-buffer-size=4 -Wformat -Werror=format-security  $(INCLUDES)

LIBS = -lseccomp -lfreetype
CFLAGS = -O3 -Wall -march=native -Werror -fPIC -fstack-protector --param=ssp-buffer-size=4 -Wformat -Werror=format-security  $(INCLUDES)

# for those without seccomp:
#LIBS = -lfreetype
#CFLAGS = -DWITHOUT_SECCOMP -O3 -Wall -march=native -Werror -fPIC -fstack-protector --param=ssp-buffer-size=4 -Wformat -Werror=format-security  $(INCLUDES)

LDFLAGS = -Wl,--gc-sections -Wl,-z,relro,-PIE -fPIC

objs = fb.o

all : monky

monky : monky.c $(objs)
	$(CC) $(CFLAGS) $(LDFLAGS) -o monky monky.c $(objs) $(LIBS)
	sudo chown root.root monky
	sudo chmod u+s monky
