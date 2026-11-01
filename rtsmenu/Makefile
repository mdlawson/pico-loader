# The rts menu run by the arm9 at RTS_BASE (common/sharedMemory.h).
# Output: rtsmenu9.bin, to be copied to /_pico/ on the sd card.
PREFIX ?= arm-none-eabi-
rtsmenu9.bin: rtsmenu9.elf
	$(PREFIX)objcopy -O binary $< $@
rtsmenu9.elf: rtsmenu9.s
	$(PREFIX)as -mcpu=arm946e-s $< -o rtsmenu9.o
	$(PREFIX)ld -Ttext=0x02100000 -e _start rtsmenu9.o -o $@
clean:
	rm -f rtsmenu9.o rtsmenu9.elf rtsmenu9.bin
