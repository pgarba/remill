# Copyright (c) 2025
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed in the License.
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF
# ANY KIND, either express or implied.  See the License for the specific
# language governing permissions and limitations under the License.

# End-to-end execution test for flat-lifted function results (64-bit only).
#
# Lifts two fixed byte blobs in --flat mode, links each lifted function
# against a C harness that stubs the terminal __remill_flat_jump dispatch and
# drives the function with concrete inputs, then checks the captured RAX
# (the function's `ret` result) against a reference implementation.
#
# Guards two bugs that made the `ret` result unobservable:
#   1. a position-based snapshot-clobber removal deleting the computed
#      register-destination store, and
#   2. __remill_compare_neq being inlined as !x (inverting every JNZ).

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  enable_testing()

  set(REMILL_FLATRET_LIFT remill-lift-${REMILL_LLVM_VERSION})

  add_test(
    NAME flat_ret
    COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_LIST_DIR}/run_flat_ret.py
            $<TARGET_FILE:${REMILL_FLATRET_LIFT}>
            ${CMAKE_BC_COMPILER}
            ${CMAKE_CURRENT_LIST_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/work
  )
  set_tests_properties(flat_ret PROPERTIES
    TIMEOUT 180
    LABELS "flat"
  )

  if (TARGET test_dependencies)
    add_dependencies(test_dependencies ${REMILL_FLATRET_LIFT})
  endif()
endif()
