EXECBIN  = httpserver
SOURCES  = $(EXECBIN).c
OBJECTS  = $(SOURCES:%.c=%.o)
FORMATS  = $(SOURCES:%.c=%.fmt)

O1 = queue
O2 = rwlock

CC       = clang
FORMAT   = clang-format
CFLAGS   = -Wall -Werror -Wextra -Wpedantic -Wstrict-prototypes

.PHONY: all clean format

all: $(EXECBIN)

$(EXECBIN): $(OBJECTS) $(O1).o $(O2).o
	$(CC) -o $@ $^

$(O1).o: $(O1).c
	$(CC) $(CFLAGS) -c $<

$(O2).o: $(O2).c
	$(CC) $(CFLAGS) -c $<


%.o : %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(EXECBIN) $(OBJECTS) $(O1).o $(O2).o

format: $(FORMATS)

%.fmt: %.c
	$(FORMAT) -i $<
	touch $@
