CC = cc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lcurl -lsqlite3 -ldl -lm
TARGET = dsco
SRCS = main.c agent.c llm.c tools.c json_util.c ast.c swarm.c tui.c md.c baseline.c setup.c crypto.c eval.c pipeline.c plugin.c semantic.c ipc.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.PHONY: all clean install
