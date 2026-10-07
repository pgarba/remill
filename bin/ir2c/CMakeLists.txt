# remill-ir2c: print a lifted LLVM function as readable C (for SICE's !LIFT C;
# see bin/ir2c/IR2C.cpp).

set(REMILL_IR2C remill-ir2c-${REMILL_LLVM_VERSION})

add_executable(${REMILL_IR2C}
  IR2C.cpp
)

target_link_libraries(${REMILL_IR2C} PRIVATE remill)

if(REMILL_ENABLE_INSTALL_TARGET)
  install(
    TARGETS ${REMILL_IR2C}
    RUNTIME DESTINATION "${REMILL_INSTALL_BIN_DIR}"
  )
endif()
