wayimg: wayimg.c
	cc -Wall -Wextra -ggdb -o ./wayimg -lwayland-client -lrt -lm ./xdg-shell-protocol.c ./wlr-layer-shell-unstable-v1.c ./wayimg.c

run: wayimg
	./wayimg
	
clean:
	rm -f wayimg

.PHONY: run clean	
