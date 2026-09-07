# Flat opcode wrapper generator.
# Transforms all ISEL opcodes in amd64.bc into flat-ABI wrappers,
# then runs SROA to scalarize them.
#
# Output: amd64_flat.bc (alongside amd64.bc)

project(remill-flat-gen)
cmake_minimum_required(VERSION 3.2)

set(REMILL_FLAT_GEN remill-flat-gen-${REMILL_LLVM_VERSION})

add_executable(${REMILL_FLAT_GEN}
  flat_gen.cpp
)

target_link_libraries(${REMILL_FLAT_GEN} PRIVATE remill)
target_include_directories(${REMILL_FLAT_GEN} SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Custom target: generate amd64_flat.bc from amd64.bc
set(FLAT_BC_OUTPUT "${CMAKE_BINARY_DIR}/lib/Arch/X86/Runtime/amd64_flat.bc")
# Base input is the flat-ABI runtime (REMILL_FLAT_ABI=1): its ISELs already
# do RSP/RBP access via pointers so the compiler emits GEPs.
set(BASE_BC_INPUT "${CMAKE_BINARY_DIR}/lib/Arch/X86/Runtime/amd64_flat_source.bc")

add_custom_target(flat-bitcode
  COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_CURRENT_BINARY_DIR}/${REMILL_FLAT_GEN}
    ${BASE_BC_INPUT}
    ${FLAT_BC_OUTPUT}
  DEPENDS ${REMILL_FLAT_GEN} amd64_flat_source
  COMMENT "Generating flat-ABI opcode wrappers (amd64_flat.bc)"
  VERBATIM
)
