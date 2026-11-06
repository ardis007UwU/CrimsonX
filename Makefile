OS_NAME := crimsonx

CC := x86_64-elf-gcc
LD := x86_64-elf-ld
NASM := nasm

ISO_DIR := dist/x86_64
BUILD_DIR := build
ISO_OUT := $(ISO_DIR)/$(OS_NAME).iso
KERNEL_BIN := $(ISO_DIR)/kernel.bin

kernel_source_files := $(shell find src/impl/kernel -name '*.c')
kernel_object_files := $(patsubst src/impl/kernel/%.c, build/kernel/%.o, $(kernel_source_files))

x86_64_c_source_files := $(shell find src/impl/x86_64 -name '*.c')
x86_64_c_object_files := $(patsubst src/impl/x86_64/%.c, build/x86_64/%.o, $(x86_64_c_source_files))

x86_64_asm_source_files := $(shell find src/impl/x86_64 -name '*.asm')
x86_64_asm_object_files := $(patsubst src/impl/x86_64/%.asm, build/x86_64/%.o, $(x86_64_asm_source_files))

x86_64_object_files := $(x86_64_c_object_files) $(x86_64_asm_object_files)
all_object_files := $(kernel_object_files) $(x86_64_object_files)

$(kernel_object_files): build/kernel/%.o : src/impl/kernel/%.c
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf -ffreestanding $< -o $@

$(x86_64_c_object_files): build/x86_64/%.o : src/impl/x86_64/%.c
	mkdir -p $(dir $@)
	$(CC) -c -I src/intf -ffreestanding $< -o $@

$(x86_64_asm_object_files): build/x86_64/%.o : src/impl/x86_64/%.asm
	mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

.PHONY: build-x86_64 run clean

build-x86_64: $(all_object_files)
	mkdir -p $(ISO_DIR)
	$(LD) -n -o $(KERNEL_BIN) -T targets/x86_64/linker.ld $(all_object_files)
	cp $(KERNEL_BIN) targets/x86_64/iso/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o $(ISO_OUT) targets/x86_64/iso

run: build-x86_64
	qemu-system-x86_64 -cdrom $(ISO_OUT)

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)