OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

KERNEL_VMA = 0xffffffff80000000;
KERNEL_LMA = 0x0000000000200000;

PHDRS
{
	text PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = KERNEL_VMA;
	__kernel_virt_start = .;
	__kernel_phys_start = KERNEL_LMA;

	.text : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA) {
		*(.text.entry)
		*(.text .text.*)
	} :text

	. = ALIGN(0x1000);
	.rodata : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA) {
		*(.rodata .rodata.*)
	} :rodata

	. = ALIGN(0x1000);
	.data : AT(ADDR(.data) - KERNEL_VMA + KERNEL_LMA) {
		*(.data .data.*)
	} :data

	. = ALIGN(0x1000);
	.bss : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA) {
		*(COMMON)
		*(.bss .bss.*)
	} :data

	. = ALIGN(0x1000);
	__kernel_virt_end = .;
	__kernel_phys_end = KERNEL_LMA + (__kernel_virt_end - __kernel_virt_start);

	/DISCARD/ : {
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame.*)
	}
}

