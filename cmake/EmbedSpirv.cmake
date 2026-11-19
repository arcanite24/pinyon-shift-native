# cmake -DINPUT=<file.spv> -DOUTPUT=<file.h> -DNAME=<symbol> -P EmbedSpirv.cmake
# Writes the SPIR-V words as a C++ array.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR remainder "${length} % 8")
if(NOT remainder EQUAL 0)
  message(FATAL_ERROR "${INPUT} is not a whole number of 32-bit words")
endif()
set(words "")
set(at 0)
while(at LESS length)
  # Little-endian words: bytes b0 b1 b2 b3 -> 0xb3b2b1b0.
  string(SUBSTRING "${hex}" ${at} 2 b0)
  math(EXPR at1 "${at} + 2")
  string(SUBSTRING "${hex}" ${at1} 2 b1)
  math(EXPR at2 "${at} + 4")
  string(SUBSTRING "${hex}" ${at2} 2 b2)
  math(EXPR at3 "${at} + 6")
  string(SUBSTRING "${hex}" ${at3} 2 b3)
  string(APPEND words "0x${b3}${b2}${b1}${b0}u,")
  math(EXPR at "${at} + 8")
  math(EXPR column "${at} % 64")
  if(column EQUAL 0)
    string(APPEND words "\n")
  endif()
endwhile()
file(WRITE "${OUTPUT}"
  "// Generated from ${INPUT}; do not edit.\n#pragma once\n#include <cstdint>\n"
  "inline constexpr uint32_t ${NAME}[] = {\n${words}};\n")
