# Host-side tests: no Vita SDK required.
test: test_core
	./test_core
SAN ?=
, := ,
test_core: test_core.c ../src/core.h
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-function -g $(if $(SAN),-fsanitize=address$(,)undefined) -o $@ test_core.c
clean:
	rm -f test_core
.PHONY: test clean
