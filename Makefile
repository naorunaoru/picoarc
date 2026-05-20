.PHONY: build build-release build-debug flash flash-debug monitor run

build:
	./picoarc build

build-release:
	./picoarc build release

build-debug:
	./picoarc build debug

flash:
	./picoarc flash

flash-debug:
	./picoarc flash debug

monitor:
	./picoarc monitor

run:
	./picoarc run
