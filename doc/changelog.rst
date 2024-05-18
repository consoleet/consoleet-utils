1.5 (2024-05-18)
================

* palcomp: add XFCE4/Termux palette file loader
  (`palcomp loadpal=somexfce4.theme`)
* palcomp: rename commands "emit"->"xfce", "stat"->"lch"
* palcomp: add "eq" and "loeq" commands for brightness equalization
* palcomp: replace "cxr" command by a APCA-based contrast derivation
  ("cxa" command)
* cp437x: delete wrong mapping from U+25B6,25B7 to 0x1e


1.4 (2023-09-24)
================

* vfontas: add an -overstrike option
* palcomp: have the "ct" command exercise AIX color codes too
* palcomp: compute "cxl" and "cxr" tables for bright background colors
  as well


1.3 (2023-08-14)
================

This release contains command to analyze and adjust contrast.

* palcomp: delete ``rgb`` and ``lch`` subcommands (make them implicit)
* palcomp: add ``ct`` and ``ct256`` subcommands to display the color table
* palcomp: add ``cxl`` and ``cxr`` subcommands for contrast analysis
* palcomp: add ``loeq`` subcommand for contrast adjustment


1.2 (2023-07-23)
================

This release saw work on the palette composition utility.

Enhancements:

* palcomp: support for tint via new "hsltint", "lchtint" or "hueset"
  subcommands (this allows for generating Amber/Green CRT looks and then some)
* palcomp: brightness manipulatino support with "litadd", "litmul", "litset"
* palcomp: auxiliary helper commands: "b0", "fg", "bg", "stat"
* palcomp: new starting palette "win"


1.1 (2022-11-22)
================

Enhancements:

* vfontas: add -cpisep option

Fixes:

* vfontas: decode CPEH and CPIH file offsets as segment offsets instead
* vfontas: avoid crash when using a unimap pointing to nonexisting glyphs
