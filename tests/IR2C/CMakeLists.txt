# remill-ir2c: lifted IR -> C (bin/ir2c). Real SICE !LIFT traces and a plain
# function, checked for expected C fragments and compiled with -Werror.

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  enable_testing()

  set(REMILL_IR2C remill-ir2c-${REMILL_LLVM_VERSION})

  add_test(
    NAME ir2c
    COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_LIST_DIR}/run_ir2c.py
            $<TARGET_FILE:${REMILL_IR2C}>
            ${CMAKE_BC_COMPILER}
            ${CMAKE_CURRENT_LIST_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/work
  )
  set_tests_properties(ir2c PROPERTIES
    TIMEOUT 120
    LABELS "flat"
  )

  if (TARGET test_dependencies)
    add_dependencies(test_dependencies ${REMILL_IR2C})
  endif()
endif()
