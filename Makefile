
OPT = -march=native -O3 -Wall -Wextra -Wno-strict-aliasing -Wno-unused-parameter -Werror

rss: table.h rss.c Makefile
	gcc --std=gnu99 -DTUPLE_NUM=0x1000000 -DTUPLE_LEN=3 $(OPT) -o rss rss.c

clean:
	rm -rf rss

test: rss
	./rss base sup sup2 sup3 dpdk dpdk2 dpdk3

