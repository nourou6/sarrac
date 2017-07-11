

# 
# if rabbitmq library is provided by SSM package, RABBITMQC_HOME is required. 
# 
ifdef RABBITMQ_HOME
RABBIT_LIBDIR = ${RABBITMQC_HOME}/lib
RABBIT_INCDIR = -I${RABBITMQC_HOME}/include
RABBIT_LINK = -Wl,-rpath,${RABBIT_LIBDIR} -L${RABBIT_LIBDIR}
SARRA_LIBDIR = ${CURDIR}
SARRA_LINK = -Wl,-rpath,${SARRA_LIBDIR} -L${SARRA_LIBDIR}
endif

# If rabbitmq library is only built (not installed) then set RABBIT_BUILD
ifdef RABBIT_BUILD
RABBIT_LIBDIR=${RABBIT_BUILD}/build/librabbitmq
RABBIT_INCDIR = -I${RABBIT_BUILD}/librabbitmq
RABBIT_LINK = -Wl,-rpath,${RABBIT_LIBDIR} -L${RABBIT_LIBDIR}
SARRA_LIBDIR = ${CURDIR}
SARRA_LINK = -Wl,-rpath,${SARRA_LIBDIR} -L${SARRA_LIBDIR}
endif

# if neither variable is set, then it is assumed to be available from default environment.

CC = gcc
CFLAGS = -fPIC -g -std=gnu99

SARRA_OBJECT = libsarra.so.1.0.0 sr_context.o sr_config.o sr_event.o sr_credentials.o
EXT_LIB = -lrabbitmq -luriparser -lcrypto -lc
SHARED_LIB = libsrshim.so.1 -o libsrshim.so.1.0.0 libsrshim.c libsarra.so.1.0.0


all: 
	$(CC) $(CFLAGS) -c -Wall libsrshim.c
	$(CC) $(CFLAGS) -c -Wall sr_credentials.c
	$(CC) $(CFLAGS) -c -Wall sr_event.c
	$(CC) $(CFLAGS) -c -Wall sr_config.c
	$(CC) $(CFLAGS) -c -Wall $(RABBIT_INCDIR) sr_context.c
	$(CC) $(CFLAGS) -shared -Wl,-soname,libsarra.so.1 -o $(SARRA_OBJECT) -ldl $(RABBIT_LINK) $(EXT_LIB)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(SHARED_LIB) -ldl $(SARRA_LINK) $(RABBIT_LINK) $(EXT_LIB)
	if [ ! -f libsarra.so ]; \
	then \
		ln -s libsarra.so.1.0.0 libsarra.so ; \
	fi;
	if [ ! -f libsarra.so.1 ]; \
	then \
		ln -s libsarra.so.1.0.0 libsarra.so.1 ; \
	fi;
	$(CC) $(CFLAGS) -o sr_configtest sr_configtest.c -lsarra $(SARRA_LINK) -lrabbitmq -luriparser -lcrypto
	$(CC) $(CFLAGS) -o sr_cpost sr_cpost.c -lsarra $(SARRA_LINK) $(RABBIT_LINK) -lrabbitmq -luriparser -lcrypto


install:
	@mkdir build build/bin build/lib build/include
	@mv *.so build/lib
	@mv *.so.* build/lib
	@mv sr_cpost build/bin
	@cp *.h build/include/

clean:
	@rm -f *.o *.so *.so.* sr_cpost sr_configtest
	@rm -rf build
