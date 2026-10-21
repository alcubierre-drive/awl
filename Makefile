.PHONY: all clean

all: build
	ninja -C build
	ninja -C build install

build:
	mkdir -p build
	meson setup --reconfigure --prefix=$(HOME)/software . build

clean:
	rm -rf build
