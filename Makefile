CC=gcc
LD=ld
CFLAGS=-fms-extensions -DTEST -Wall `pkg-config --cflags --libs libxml-2.0`

TARGET=audio-scheduler

SRCS=cfg_handler.c \
     pls_handler.c \
     utils.c \
     scheduler.c \
     main.c \
     config_schema.o \
     $(NULL)

TOCLEAN=config_schema.o \
        $(NULL)

debug: clean $(SRCS)
	$(CC) $(CFLAGS) -g -DDEBUG -o $(TARGET) $(SRCS)

stable: clean $(SRCS)
	$(CC) $(CFLAGS) -O2 -o $(TARGET) $(SRCS)

clean:
	rm -vfr *~ $(TOCLEAN) $(TARGET)

%.o: %.xsd
	$(LD) -r -b binary -o $@ $<
