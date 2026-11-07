C_SOURCES := $(shell find src/impl -name '*.c')
C_OBJECTS := $(patsubst src/impl/%.c, build/%.o, $(C_SOURCES))

ASM_SOURCES := $(shell find src/impl -name '*.asm')
ASM_OBJECTS := $(patsubst src/impl/%.asm, build/%.o, $(ASM_SOURCES))

# Userland demo binary: built first, incbin'd by src/impl/x86_64/hello_data.asm
HELLO_SRC := src/tools/hello/hello.asm src/tools/hello/hello.ld
HELLO_ELF := build/app/hello.elf

$(HELLO_ELF): $(HELLO_SRC) | build/app
	@mkdir -p $(dir $@)
	nasm -f elf64 src/tools/hello/hello.asm -o build/app/hello.o
	x86_64-elf-ld -T src/tools/hello/hello.ld build/app/hello.o -o $@

build/app:
	mkdir -p build/app

$(C_OBJECTS): build/%.o : src/impl/%.c
	@mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I src/intf -ffreestanding -mno-red-zone $< -o $@

$(ASM_OBJECTS): $(HELLO_ELF)

# hello_data.asm resolves its incbin against build/app.
build/x86_64/hello_data.o: NASM_EXTRA = -I build/app

$(ASM_OBJECTS): build/%.o : src/impl/%.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $(NASM_EXTRA) $< -o $@

# Host-side unit tests for the memory managers (needs only a normal gcc).
# Runs vmm.c / pmm.c as ordinary programs under AddressSanitizer + UBSan.
# BUILD_HOST is overridable: `make test-host BUILD_HOST=/tmp/...` when build/ is
# owned by the docker user.
BUILD_HOST ?= build/host
.PHONY: test-host
test-host:
	@mkdir -p $(BUILD_HOST)
	gcc -g -Wall -Wextra -fsanitize=address,undefined -I src/intf -DVMM_HOST_TEST \
		tests/host/vmm_test.c src/impl/mm/vmm.c -o $(BUILD_HOST)/vmm_test
	gcc -g -Wall -Wextra -fsanitize=address,undefined -I src/intf \
		tests/host/pmm_test.c src/impl/kernel/pmm.c -o $(BUILD_HOST)/pmm_test
	gcc -g -Wall -Wextra -fsanitize=address,undefined -I src/intf -DVMM_HOST_TEST \
		tests/host/elf_test.c src/impl/kernel/elf64.c src/impl/mm/vmm.c -o $(BUILD_HOST)/elf_test
	gcc -g -Wall -Wextra -fsanitize=address,undefined -I src/intf \
		tests/host/crimsonfs_test.c src/impl/fs/crimsonfs.c src/impl/fs/vfs.c src/impl/kernel/blkdev.c src/impl/kernel/string.c \
		-o $(BUILD_HOST)/crimsonfs_test
	$(BUILD_HOST)/vmm_test
	$(BUILD_HOST)/pmm_test
	$(BUILD_HOST)/elf_test
	$(BUILD_HOST)/crimsonfs_test

.PHONY: build-x86_64
build-x86_64: $(C_OBJECTS) $(ASM_OBJECTS)
	@mkdir -p dist/x86_64
	x86_64-elf-ld -n -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(C_OBJECTS) $(ASM_OBJECTS)
	@mkdir -p targets/x86_64/iso/boot
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o dist/x86_64/crimsonx.iso targets/x86_64/iso
