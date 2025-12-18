main:
	mkdir -p build
	gcc -Wall -Wextra -o build/main main.c && ./build/main
debug:
	mkdir -p build
	gcc -g -Wall -Wextra -o build/main main.c && lldb ./build/main
gen:
	mkdir -p build
	gcc -Wall -Wextra -o build/gen gen.c && ./build/gen 10
clean:
	rm -rf build
