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
