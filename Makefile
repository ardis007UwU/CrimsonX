C_SOURCES := $(shell find src/impl -name '*.c')
C_OBJECTS := $(patsubst src/impl/%.c, build/%.o, $(C_SOURCES))

ASM_SOURCES := $(shell find src/impl -name '*.asm')
ASM_OBJECTS := $(patsubst src/impl/%.asm, build/%.o, $(ASM_SOURCES))

$(C_OBJECTS): build/%.o : src/impl/%.c
	@mkdir -p $(dir $@)
	x86_64-elf-gcc -c -I src/intf -ffreestanding $< -o $@

$(ASM_OBJECTS): build/%.o : src/impl/%.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

.PHONY: build-x86_64
build-x86_64: $(C_OBJECTS) $(ASM_OBJECTS)
	@mkdir -p dist/x86_64
	x86_64-elf-ld -n -o dist/x86_64/kernel.bin -T targets/x86_64/linker.ld $(C_OBJECTS) $(ASM_OBJECTS)
	@mkdir -p targets/x86_64/iso/boot
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o dist/x86_64/crimsonx.iso targets/x86_64/iso