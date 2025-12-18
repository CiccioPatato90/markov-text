run:
	gcc -Wall -Wextra -o main main.c && ./main
debug:
	gcc -g -Wall -Wextra -o main main.c && lldb ./main
clean:
	rm -f main
