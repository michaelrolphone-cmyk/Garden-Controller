XTENSA_GCC ?= xtensa-esp32s3-elf-gcc

.PHONY: packages
packages:
	python3 scripts/build_packages.py --gcc "$(XTENSA_GCC)"
