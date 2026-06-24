.PHONY: build build-release build-debug flash flash-debug monitor run fab

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

# Export a JLCPCB assembly package (Gerbers, BOM, CPL).
# Usage: make fab              — strict DRC gate (aborts on any violation)
#        make fab FAB_ARGS=--skip-drc      — bypass DRC (outputs tagged -dirty)
fab:
	python3 hardware/fab.py $(FAB_ARGS)
