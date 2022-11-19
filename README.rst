consoleet-utils
===============

This is a set of utilities for manipulating terminal fonts and colors.

A key component is *vfontas*, which can read/write bitmap fonts from/to a
number of formats and transform the glyphs in various ways. vfontas is able to
generate outline fonts from bitmapped fonts, including a high-quality mode that
upscales based on outline rather than pixel blocks, setting it apart from
scalers like xBRZ or potrace.
