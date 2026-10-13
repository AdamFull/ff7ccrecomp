#pragma once

#include <string>
#include <string_view>

#include "ff7cc/formats/atel.hpp"

namespace ff7cc::atel {

// Escapes a name-pool string as a double-quoted literal. Printable ASCII is kept; everything else
// (Shift-JIS text, control characters) becomes \xNN so the listing stays lossless.
std::string quote(std::string_view s);

// Human-readable listing of a module's bytecode: method entry points are labelled "Object.method",
// call targets "sub_XXXX", jump targets "L_XXXX"; name operands are shown as string literals.
std::string disassemble(const Module& module);

}  // namespace ff7cc::atel
