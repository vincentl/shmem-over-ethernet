/*
 * osoe's symmetric statics (design §4.3): oshcc passes this to the linker, and
 * it gathers the program's writable data - every .data and .bss input section,
 * whichever object or archive member it comes from, and x86-64's large-model
 * .ldata and .lbss - into two output sections that begin and end on the
 * linker's largest page, bracketed by the symbols libosoe looks up at
 * shmem_init. It maps each PE's own shared object over them, so that every
 * global and static variable of the program is a symmetric object, addressed
 * by its offset in the pair.
 *
 * An augmentation, not a whole script: INSERT keeps the linker's default
 * layout and places each section before the default one of its kind. Its
 * statements are matched before the default script's, so .data.* also takes
 * .data.rel.ro - read-only data with relocations, and GNU ld's copies of
 * shared libraries' read-only objects - which then lose RELRO's protection.
 * With one object per PE nothing gathered is shared between processes but
 * through a SHMEM routine that names it, so that costs hardening, not
 * correctness. Thread-local data (.tdata, .tbss) has other names and stays
 * out, as do the GOT, the constructor tables and GNU ld's copies of shared
 * libraries' writable variables (.dynbss: stdout, environ).
 *
 * Tested with GNU ld. lld accepts it, and puts those copies in .bss, where
 * they are gathered; gold and mold do not take INSERT.
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
    *(.ldata .ldata.* .gnu.linkonce.l.*)
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
    *(.lbss .lbss.* .gnu.linkonce.lb.*)
    *(COMMON LARGE_COMMON)
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __osoe_statics_bss_end = .;
  }
}
INSERT BEFORE .bss;
