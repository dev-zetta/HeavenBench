// stb_vorbis.h — public API header for the vendored stb_vorbis decoder.
//
// stb_vorbis (public domain, http://nothings.org/stb_vorbis/) ships as a
// single `stb_vorbis.c` that contains BOTH its public declarations and its
// implementation. The upstream-recommended way to obtain just the
// declarations is to define STB_VORBIS_HEADER_ONLY before including the .c,
// which compiles only the header section (the block between
// STB_VORBIS_INCLUDE_STB_VORBIS_H and "HEADER ENDS HERE"). Doing that here —
// rather than hand-copying the prototypes into a separate header — keeps this
// file in lockstep with the vendored decoder so the declarations can never
// drift from the implementation.
//
// Consumers include this header to get stb_vorbis_decode_filename,
// stb_vorbis_decode_memory, and the rest of the public API; exactly one
// translation unit (third_party/stb_vorbis.c, listed in ENGINE_SOURCES)
// provides the implementation.
//
// Vendored verbatim; do not modify stb_vorbis.c after vendoring.
#ifndef UGX_STB_VORBIS_H
#define UGX_STB_VORBIS_H

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#endif // UGX_STB_VORBIS_H
