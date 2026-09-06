/*
 * Copyright (c) 2024 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// Flat ("unfolded") x86-64 architectural state.
//
// Remill's default lifting represents the machine state as a pointer to the
// large `State` struct. Every register access in the lifted code is then a
// load/store through that pointer, which keeps the whole state pinned in
// memory and defeats LLVM's scalar replacement of aggregates (SROA): registers
// never become SSA values, RSP never folds to a constant, and branch
// conditions computed through the state cannot be proven.
//
// This header defines a *flat* view of the state in which every register is an
// individual field. A lifted function built in "flat mode" takes a pointer to
// this structure -- `X86FlatState *` -- and treats every field as an in-out
// reference: it reads the current register values through the pointer, runs
// the (unchanged) `State &`-based semantics against a *local* `State` that it
// materializes, and writes the updated values back through the same pointer.
//
// Because the local `State` is a plain alloca whose only uses are the inlined
// field loads/stores and the inlined ISEL calls, SROA promotes it to SSA
// values, giving the same register-in-SSA code shape as a hand-written
// unfolded function. The caller simply owns one `X86FlatState` and threads it
// across block boundaries; there is no separate output parameter.
//
// The field order below is the canonical flat-state layout. The
// `__remill_flat_state_*` helpers map it to/from `State`.

#include "remill/Arch/Runtime/Types.h"

struct X86FlatState {
  // General purpose registers.
  addr_t rax;
  addr_t rbx;
  addr_t rcx;
  addr_t rdx;
  addr_t rsi;
  addr_t rdi;
  addr_t rsp;
  addr_t rbp;
  addr_t r8;
  addr_t r9;
  addr_t r10;
  addr_t r11;
  addr_t r12;
  addr_t r13;
  addr_t r14;
  addr_t r15;
  addr_t rip;

  // Segment bases.
  addr_t ss_base;
  addr_t gs_base;
  addr_t cs_base;
  addr_t fs_base;

  // Arithmetic flags (byte mirror of the packed RFLAGS).
  uint8_t cf;
  uint8_t pf;
  uint8_t af;
  uint8_t zf;
  uint8_t sf;
  uint8_t df;
  uint8_t of;

  // MMX registers, 64-bit view.
  uint64_t mm0;
  uint64_t mm1;
  uint64_t mm2;
  uint64_t mm3;
  uint64_t mm4;
  uint64_t mm5;
  uint64_t mm6;
  uint64_t mm7;

  // XMM registers, 128-bit view (xmm0-xmm15).
  vec128_t xmm[16];
};

// Number of register fields carried across a flat boundary (everything in
// `X86FlatState`).
enum : unsigned { kX86FlatStateNumFields = 17 + 4 + 7 + 8 + 16 };

// Flat-mode jump. Unlike `__remill_jump` (which takes a `State &`), this takes
// the lifted function's register pointer arguments *directly* -- the same 51
// in-out pointers the flat function was called with, in the same order as
// `X86FlatState`. The runtime reads the final register values through those
// pointers (the flat function has already written them back via
// `__remill_flat_state_store`) and dispatches to the next block. This avoids
// rebuilding a `State` struct (51 loads) at every block boundary.
//
// Implemented by the remill runtime (a separate component); declared here so
// the flat lifter can emit a tail call to it.
#if 64 == ADDRESS_SIZE_BITS
// ABI v2: registers are passed by value. PC/NEXT_PC remain pointers (threaded
// between blocks). The runtime stores the by-value register args into the
// caller's state and dispatches to the next block.
extern "C" Memory *__remill_flat_jump(
    addr_t *pc, Memory *memory, addr_t *next_pc,
    // 17 GPRs (by value).
    addr_t rax, addr_t rbx, addr_t rcx, addr_t rdx,
    addr_t rsi, addr_t rdi, addr_t rsp, addr_t rbp,
    addr_t r8, addr_t r9, addr_t r10, addr_t r11,
    addr_t r12, addr_t r13, addr_t r14, addr_t r15,
    addr_t rip,
    // 4 segment bases (by value).
    addr_t ss_base, addr_t gs_base, addr_t cs_base, addr_t fs_base,
    // 7 flags (by value).
    uint8_t cf, uint8_t pf, uint8_t af, uint8_t zf,
    uint8_t sf, uint8_t df, uint8_t of,
    // 8 MMX (by value).
    uint64_t mm0, uint64_t mm1, uint64_t mm2, uint64_t mm3,
    uint64_t mm4, uint64_t mm5, uint64_t mm6, uint64_t mm7,
    // 16 XMM (by value).
    vec128_t xmm0, vec128_t xmm1, vec128_t xmm2, vec128_t xmm3,
    vec128_t xmm4, vec128_t xmm5, vec128_t xmm6, vec128_t xmm7,
    vec128_t xmm8, vec128_t xmm9, vec128_t xmm10, vec128_t xmm11,
    vec128_t xmm12, vec128_t xmm13, vec128_t xmm14, vec128_t xmm15,
    // 8 X87 ST (by value, i128 = 80-bit float80 padded to 128 bits).
    uint64_t st0_lo, uint64_t st0_hi,
    uint64_t st1_lo, uint64_t st1_hi,
    uint64_t st2_lo, uint64_t st2_hi,
    uint64_t st3_lo, uint64_t st3_hi,
    uint64_t st4_lo, uint64_t st4_hi,
    uint64_t st5_lo, uint64_t st5_hi,
    uint64_t st6_lo, uint64_t st6_hi,
    uint64_t st7_lo, uint64_t st7_hi);
#endif  // 64 == ADDRESS_SIZE_BITS
