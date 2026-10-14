#pragma once


#include "ff7cc/formats/atel.hpp"
#include "core/foundation/strings/utf8_string.h"

namespace ff7cc::atel {

// Assembles a listing in the format produced by disassemble() (typically after editing) and stores the
// result in module.code.
//
//   - Lines are `label:` or `[offset] mnemonic [operands]`; the hex offset column is ignored, and text after
//     ';' (outside string literals) is a comment.
//   - Name operands are string literals. A string already in module.names reuses its first occurrence;
//     new strings are appended to the pool, so existing name offsets never move.
//   - Every method entry point is re-derived from its "Object.method" label, so code may grow or shrink.
//
// Throws FormatError (with the line number) on any syntax or resolution error; `module` is left unchanged then.
void assemble_into(Module& module, nx::string_view listing);

}  // namespace ff7cc::atel
