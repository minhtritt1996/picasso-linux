# Thin wrapper: every target is a build.sh subcommand or a gate script, so the
# behaviour lives in one place. RAM boot only; nothing here writes flash.
.PHONY: help build kernel loader bootimg bootimg-auto check preflight clean

help:
	@./build.sh

build: kernel loader bootimg

kernel:
	./build.sh linux

loader:
	./build.sh loader

bootimg:
	./build.sh bootimg

bootimg-auto:
	./build.sh bootimg-auto

check:
	./build.sh check

preflight:
	scripts/preflight.sh

clean:
	./build.sh clean
