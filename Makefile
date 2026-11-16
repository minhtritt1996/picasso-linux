# Thin wrapper: every target is a build.sh subcommand or a gate script, so the
# behaviour lives in one place. RAM boot only; nothing here writes flash.
.PHONY: help build kernel loader bootimg bootimg-luma check preflight clean

help:
	@./build.sh

build: kernel loader bootimg

kernel:
	./build.sh linux

loader:
	./build.sh loader

bootimg:
	./build.sh bootimg

bootimg-luma:
	./build.sh bootimg-luma

check:
	./build.sh check

preflight:
	tools/preflight.sh

clean:
	./build.sh clean
