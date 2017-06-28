all:
	ninja-build -C build

install:
	ninja-build -C build install

clean:
	ninja-build -C build clean
