/*
 * osoe's symmetric statics (design §4.3): oshcc passes this to the linker, and
 * it gathers the program's writable data - every .data and .bss input section,
 * whichever object or archive member it comes from - into two output sections
 * that begin and end on the linker's largest page, bracketed by the symbols
 * libosoe looks up at shmem_init. It maps each PE's own shared object over
 * them, so that every global and static variable of the program is a symmetric
 * object, addressed by its offset in the pair.
 *
 * An augmentation, not a whole script: INSERT keeps the linker's default
 * layout and places each section before the default one of its kind. What
 * stays out: thread-local data (.tdata and .tbss are other names), the GOT,
 * the constructor tables, and the copies of a shared library's variables the
 * linker makes for a program that is not position-independent (.dynbss) -
 * none of them a variable of the program's, and each holding addresses of this
 * process's own.
 *
 * The ends are aligned as well as the starts, so no other section shares a page
 * with the region and the mapping over it hides nothing else.
 */
SECTIONS
{
  .osoe_statics_data : ALIGN(CONSTANT(MAXPAGESIZE))
  {
    __osoe_statics_data_start = .;
    *(.data .data.* .gnu.linkonce.d.*)
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __osoe_statics_data_end = .;
  }
}
INSERT BEFORE .data;

SECTIONS
{
  .osoe_statics_bss : ALIGN(CONSTANT(MAXPAGESIZE))
  {
    __osoe_statics_bss_start = .;
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(COMMON)
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __osoe_statics_bss_end = .;
  }
}
INSERT BEFORE .bss;
