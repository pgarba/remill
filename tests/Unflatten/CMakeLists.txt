# Copyright (c) 2025
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Smoke test for the Unflatten pass: merge a small flat-block module into a
# single function with a real CFG and verify the structural invariants.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

enable_testing()

set(REMILL_UNFLATTEN_LIFT remill-lift-${REMILL_LLVM_VERSION})

add_test(
  NAME unflatten_smoke
  COMMAND Python3::Interpreter
          ${CMAKE_CURRENT_LIST_DIR}/run_smoke.py
          $<TARGET_FILE:${REMILL_UNFLATTEN_LIFT}>
          ${CMAKE_CURRENT_LIST_DIR}
)
set_tests_properties(unflatten_smoke PROPERTIES
  TIMEOUT 120
  LABELS "unflatten"
)

if(TARGET test_dependencies)
  add_dependencies(test_dependencies ${REMILL_UNFLATTEN_LIFT})
endif()
