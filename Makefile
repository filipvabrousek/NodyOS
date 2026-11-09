# NodyOS build
#
#   make            build nody.img (boot loader + kernel; keeps the NodyFS volume)
#   make run        boot it in QEMU
#   make newdisk    throw the disk image away and start with a fresh volume
#   make host       build tools/hostsim (runs the GUI stack natively, for testing)
#   make clean      remove build products (the disk image is kept)

CC      := i686-elf-gcc
LD      := i686-elf-ld
OBJCOPY := i686-elf-objcopy
NASM    := nasm
QEMU    := qemu-system-i386

BUILD   := build
IMG     := nody.img
IMG_MB  := 32

KERNEL_LBA  := 16
KERNEL_MAX  := 2032          # sectors between the kernel and the NodyFS volume

CFLAGS  := -std=gnu11 -O2 -ffreestanding -fno-builtin -nostdinc -fno-pic \
           -fno-stack-protector -fno-asynchronous-unwind-tables -fomit-frame-pointer \
           -fno-strict-aliasing -march=pentium4 -msse2 -mfpmath=sse -mstackrealign \
           -ffast-math -Wall -Wextra -Wno-unused-parameter -I. -MMD -MP

C_SRCS  := $(wildcard kernel/*.c lib/*.c gui/*.c flow/*.c apps/*.c)
OBJS    := $(BUILD)/kernel/entry.o $(C_SRCS:%.c=$(BUILD)/%.o)

QEMU_FLAGS := -drive file=$(IMG),format=raw,if=ide,index=0,media=disk \
              -m 256 -vga std -rtc base=localtime -name NodyOS

.PHONY: all run newdisk host clean

all: $(IMG)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/entry.o: kernel/entry.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/kernel.elf: $(OBJS) kernel/linker.ld
	$(LD) -m elf_i386 --no-warn-rwx-segments -T kernel/linker.ld -o $@ $(OBJS)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# the boot loader needs to know how many sectors of kernel to load
$(BUILD)/boot.bin: boot/boot.asm $(BUILD)/kernel.bin
	@s=$$(( ($$(wc -c < $(BUILD)/kernel.bin) + 511) / 512 )); \
	if [ $$s -gt $(KERNEL_MAX) ]; then echo "kernel too large: $$s sectors"; exit 1; fi; \
	echo "$(NASM) boot/boot.asm (kernel: $$s sectors)"; \
	$(NASM) -f bin -DKERNEL_SECTORS=$$s boot/boot.asm -o $@

# The image is created once; later builds only rewrite the boot loader and the
# kernel, so files saved on the NodyFS volume survive a rebuild.
$(IMG): $(BUILD)/boot.bin $(BUILD)/kernel.bin
	@test -f $@ || { echo "creating $(IMG_MB) MB disk image"; dd if=/dev/zero of=$@ bs=1048576 count=$(IMG_MB) 2>/dev/null; }
	dd if=$(BUILD)/boot.bin of=$@ conv=notrunc 2>/dev/null
	dd if=$(BUILD)/kernel.bin of=$@ bs=512 seek=$(KERNEL_LBA) conv=notrunc 2>/dev/null

# QEMU_EXTRA adds flags, e.g.  make run QEMU_EXTRA="-display none"
run: $(IMG)
	$(QEMU) $(QEMU_FLAGS) -serial stdio $(QEMU_EXTRA)

newdisk:
	rm -f $(IMG)
	$(MAKE) $(IMG)

HOST_SRCS := tools/hostsim.c $(wildcard lib/*.c gui/*.c flow/*.c apps/*.c)

host: $(BUILD)/hostsim

$(BUILD)/hostsim: $(HOST_SRCS) $(wildcard lib/*.h gui/*.h flow/*.h apps/*.h)
	@mkdir -p $(BUILD)
	cc -std=gnu11 -O1 -g -DNODY_HOST -fsanitize=address,undefined -fno-builtin -Wall -Wextra \
	   -Wno-unused-parameter -I. $(HOST_SRCS) -o $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
