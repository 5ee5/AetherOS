OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
	dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
	. = 0x1000;

	.text : {
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.gnu.hash)
	} :text

	. = ALIGN(0x1000);
	.reloc : {
		KEEP(*(.reloc))
	} :text

	. = ALIGN(0x1000);
	.data : {
		*(.data .data.*)
	} :data

	.dynamic : {
		*(.dynamic)
	} :data :dynamic

	.dynsym : {
		*(.dynsym)
	} :data

	.dynstr : {
		*(.dynstr)
	} :data

	.rela.dyn : {
		*(.rela .rela.*)
	} :data

	.bss : {
		*(.bss .bss.* COMMON)
	} :data

	/DISCARD/ : {
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame.*)
	}
}
