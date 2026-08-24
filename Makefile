.PHONY: build build-release build-debug build-uac2 build-release-uac2 build-debug-uac2 \
	flash flash-debug flash-uac2 flash-debug-uac2 monitor run run-uac2

build:
	./picoarc build

build-release:
	./picoarc build release

build-debug:
	./picoarc build debug

build-uac2:
	./picoarc build --uac uac2

build-release-uac2:
	./picoarc build release --uac uac2

build-debug-uac2:
	./picoarc build debug --uac uac2

flash:
	./picoarc flash

flash-debug:
	./picoarc flash debug

flash-uac2:
	./picoarc flash --uac uac2

flash-debug-uac2:
	./picoarc flash debug --uac uac2

monitor:
	./picoarc monitor

run:
	./picoarc run

run-uac2:
	./picoarc run --uac uac2
