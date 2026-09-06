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

// Flat-state <-> State mapping helpers for "flat mode" lifting.
//
// These are compiled into the x86 semantics bitcode and marked
// `always_inline` so that, when the bitcode is linked into a lifted module,
// they inline into the lifted function. Once inlined, the only remaining uses
// of the local `State` alloca are simple field loads/stores (plus the inlined
// ISEL calls), which SROA promotes to SSA values.
//
// The lifted function takes individual pointer arguments for each register.
// `__remill_flat_state_load` reads the current values through those pointers
// into a fresh local `State`; `__remill_flat_state_store` writes the final
// local `State` back through the same pointers.

#include "remill/Arch/X86/Runtime/FlatState.h"
#include "remill/Arch/X86/Runtime/State.h"

// Flat lifting is a 64-bit-only feature.
#if 64 == ADDRESS_SIZE_BITS

using addr_t = uint64_t;

// `__remill_mark_as_used` is provided by the runtime (declared in
// Intrinsics.cpp). Referencing `__remill_flat_jump` from a `[[gnu::used]]`
// function forces a declaration of it into the bitcode so the flat lifter can
// find it by name (mirrors `__remill_intrinsics` in Intrinsics.cpp).
extern "C" void __remill_mark_as_used(const void *);
extern "C" void __remill_flat_intrinsics(void) [[gnu::used]] {
  __remill_mark_as_used(reinterpret_cast<const void *>(&__remill_flat_jump));
}

// Entry-side mapping: read each register from its pointer into the local
// `State`. The 51 pointer arguments match the lifted function's register
// arguments in the same order.
extern "C" __attribute__((always_inline)) void __remill_flat_state_load(
    State *state,
    // 17 GPRs.
    addr_t *rax, addr_t *rbx, addr_t *rcx, addr_t *rdx,
    addr_t *rsi, addr_t *rdi, addr_t *rsp, addr_t *rbp,
    addr_t *r8, addr_t *r9, addr_t *r10, addr_t *r11,
    addr_t *r12, addr_t *r13, addr_t *r14, addr_t *r15,
    addr_t *rip,
    // 4 segment bases.
    addr_t *ss_base, addr_t *gs_base, addr_t *cs_base, addr_t *fs_base,
    // 7 flags.
    uint8_t *cf, uint8_t *pf, uint8_t *af, uint8_t *zf,
    uint8_t *sf, uint8_t *df, uint8_t *of,
    // 8 MMX.
    uint64_t *mm0, uint64_t *mm1, uint64_t *mm2, uint64_t *mm3,
    uint64_t *mm4, uint64_t *mm5, uint64_t *mm6, uint64_t *mm7,
    // 16 XMM.
    vec128_t *xmm0, vec128_t *xmm1, vec128_t *xmm2, vec128_t *xmm3,
    vec128_t *xmm4, vec128_t *xmm5, vec128_t *xmm6, vec128_t *xmm7,
    vec128_t *xmm8, vec128_t *xmm9, vec128_t *xmm10, vec128_t *xmm11,
    vec128_t *xmm12, vec128_t *xmm13, vec128_t *xmm14, vec128_t *xmm15) {

  // General purpose registers.
  state->gpr.rax.qword = *rax;
  state->gpr.rbx.qword = *rbx;
  state->gpr.rcx.qword = *rcx;
  state->gpr.rdx.qword = *rdx;
  state->gpr.rsi.qword = *rsi;
  state->gpr.rdi.qword = *rdi;
  state->gpr.rsp.qword = *rsp;
  state->gpr.rbp.qword = *rbp;
  state->gpr.r8.qword = *r8;
  state->gpr.r9.qword = *r9;
  state->gpr.r10.qword = *r10;
  state->gpr.r11.qword = *r11;
  state->gpr.r12.qword = *r12;
  state->gpr.r13.qword = *r13;
  state->gpr.r14.qword = *r14;
  state->gpr.r15.qword = *r15;
  state->gpr.rip.qword = *rip;

  // Segment bases.
  state->addr.ss_base.qword = *ss_base;
  state->addr.gs_base.qword = *gs_base;
  state->addr.cs_base.qword = *cs_base;
  state->addr.fs_base.qword = *fs_base;

  // Flags.
  state->aflag.cf = *cf;
  state->aflag.pf = *pf;
  state->aflag.af = *af;
  state->aflag.zf = *zf;
  state->aflag.sf = *sf;
  state->aflag.df = *df;
  state->aflag.of = *of;

  state->rflag.flat =
      static_cast<uint64_t>(*cf) | (1ULL << 1) |
      (static_cast<uint64_t>(*pf) << 2) |
      (static_cast<uint64_t>(*af) << 4) |
      (static_cast<uint64_t>(*zf) << 6) |
      (static_cast<uint64_t>(*sf) << 7) |
      (static_cast<uint64_t>(*df) << 10) |
      (static_cast<uint64_t>(*of) << 11);

  // MMX registers.
  state->mmx.elems[0].val.qwords.elems[0] = *mm0;
  state->mmx.elems[1].val.qwords.elems[0] = *mm1;
  state->mmx.elems[2].val.qwords.elems[0] = *mm2;
  state->mmx.elems[3].val.qwords.elems[0] = *mm3;
  state->mmx.elems[4].val.qwords.elems[0] = *mm4;
  state->mmx.elems[5].val.qwords.elems[0] = *mm5;
  state->mmx.elems[6].val.qwords.elems[0] = *mm6;
  state->mmx.elems[7].val.qwords.elems[0] = *mm7;

  // XMM registers. Use the dqwords field for direct 128-bit loads/stores
  // (avoids llvm.memcpy which defeats SROA).
  state->vec[0].xmm.dqwords.elems[0] = xmm0->dqwords.elems[0];
  state->vec[1].xmm.dqwords.elems[0] = xmm1->dqwords.elems[0];
  state->vec[2].xmm.dqwords.elems[0] = xmm2->dqwords.elems[0];
  state->vec[3].xmm.dqwords.elems[0] = xmm3->dqwords.elems[0];
  state->vec[4].xmm.dqwords.elems[0] = xmm4->dqwords.elems[0];
  state->vec[5].xmm.dqwords.elems[0] = xmm5->dqwords.elems[0];
  state->vec[6].xmm.dqwords.elems[0] = xmm6->dqwords.elems[0];
  state->vec[7].xmm.dqwords.elems[0] = xmm7->dqwords.elems[0];
  state->vec[8].xmm.dqwords.elems[0] = xmm8->dqwords.elems[0];
  state->vec[9].xmm.dqwords.elems[0] = xmm9->dqwords.elems[0];
  state->vec[10].xmm.dqwords.elems[0] = xmm10->dqwords.elems[0];
  state->vec[11].xmm.dqwords.elems[0] = xmm11->dqwords.elems[0];
  state->vec[12].xmm.dqwords.elems[0] = xmm12->dqwords.elems[0];
  state->vec[13].xmm.dqwords.elems[0] = xmm13->dqwords.elems[0];
  state->vec[14].xmm.dqwords.elems[0] = xmm14->dqwords.elems[0];
  state->vec[15].xmm.dqwords.elems[0] = xmm15->dqwords.elems[0];
}

// Exit-side mapping: write the final local `State` back through the
// individual register pointers.
extern "C" __attribute__((always_inline)) void __remill_flat_state_store(
    // 17 GPRs.
    addr_t *rax, addr_t *rbx, addr_t *rcx, addr_t *rdx,
    addr_t *rsi, addr_t *rdi, addr_t *rsp, addr_t *rbp,
    addr_t *r8, addr_t *r9, addr_t *r10, addr_t *r11,
    addr_t *r12, addr_t *r13, addr_t *r14, addr_t *r15,
    addr_t *rip,
    // 4 segment bases.
    addr_t *ss_base, addr_t *gs_base, addr_t *cs_base, addr_t *fs_base,
    // 7 flags.
    uint8_t *cf, uint8_t *pf, uint8_t *af, uint8_t *zf,
    uint8_t *sf, uint8_t *df, uint8_t *of,
    // 8 MMX.
    uint64_t *mm0, uint64_t *mm1, uint64_t *mm2, uint64_t *mm3,
    uint64_t *mm4, uint64_t *mm5, uint64_t *mm6, uint64_t *mm7,
    // 16 XMM.
    vec128_t *xmm0, vec128_t *xmm1, vec128_t *xmm2, vec128_t *xmm3,
    vec128_t *xmm4, vec128_t *xmm5, vec128_t *xmm6, vec128_t *xmm7,
    vec128_t *xmm8, vec128_t *xmm9, vec128_t *xmm10, vec128_t *xmm11,
    vec128_t *xmm12, vec128_t *xmm13, vec128_t *xmm14, vec128_t *xmm15,
    // State (last argument).
    const State *state) {

  *rax = state->gpr.rax.qword;
  *rbx = state->gpr.rbx.qword;
  *rcx = state->gpr.rcx.qword;
  *rdx = state->gpr.rdx.qword;
  *rsi = state->gpr.rsi.qword;
  *rdi = state->gpr.rdi.qword;
  *rsp = state->gpr.rsp.qword;
  *rbp = state->gpr.rbp.qword;
  *r8 = state->gpr.r8.qword;
  *r9 = state->gpr.r9.qword;
  *r10 = state->gpr.r10.qword;
  *r11 = state->gpr.r11.qword;
  *r12 = state->gpr.r12.qword;
  *r13 = state->gpr.r13.qword;
  *r14 = state->gpr.r14.qword;
  *r15 = state->gpr.r15.qword;
  *rip = state->gpr.rip.qword;

  *ss_base = state->addr.ss_base.qword;
  *gs_base = state->addr.gs_base.qword;
  *cs_base = state->addr.cs_base.qword;
  *fs_base = state->addr.fs_base.qword;

  *cf = state->aflag.cf;
  *pf = state->aflag.pf;
  *af = state->aflag.af;
  *zf = state->aflag.zf;
  *sf = state->aflag.sf;
  *df = state->aflag.df;
  *of = state->aflag.of;

  *mm0 = state->mmx.elems[0].val.qwords.elems[0];
  *mm1 = state->mmx.elems[1].val.qwords.elems[0];
  *mm2 = state->mmx.elems[2].val.qwords.elems[0];
  *mm3 = state->mmx.elems[3].val.qwords.elems[0];
  *mm4 = state->mmx.elems[4].val.qwords.elems[0];
  *mm5 = state->mmx.elems[5].val.qwords.elems[0];
  *mm6 = state->mmx.elems[6].val.qwords.elems[0];
  *mm7 = state->mmx.elems[7].val.qwords.elems[0];

  // XMM registers. Use the dqwords field for direct 128-bit loads/stores.
  xmm0->dqwords.elems[0] = state->vec[0].xmm.dqwords.elems[0];
  xmm1->dqwords.elems[0] = state->vec[1].xmm.dqwords.elems[0];
  xmm2->dqwords.elems[0] = state->vec[2].xmm.dqwords.elems[0];
  xmm3->dqwords.elems[0] = state->vec[3].xmm.dqwords.elems[0];
  xmm4->dqwords.elems[0] = state->vec[4].xmm.dqwords.elems[0];
  xmm5->dqwords.elems[0] = state->vec[5].xmm.dqwords.elems[0];
  xmm6->dqwords.elems[0] = state->vec[6].xmm.dqwords.elems[0];
  xmm7->dqwords.elems[0] = state->vec[7].xmm.dqwords.elems[0];
  xmm8->dqwords.elems[0] = state->vec[8].xmm.dqwords.elems[0];
  xmm9->dqwords.elems[0] = state->vec[9].xmm.dqwords.elems[0];
  xmm10->dqwords.elems[0] = state->vec[10].xmm.dqwords.elems[0];
  xmm11->dqwords.elems[0] = state->vec[11].xmm.dqwords.elems[0];
  xmm12->dqwords.elems[0] = state->vec[12].xmm.dqwords.elems[0];
  xmm13->dqwords.elems[0] = state->vec[13].xmm.dqwords.elems[0];
  xmm14->dqwords.elems[0] = state->vec[14].xmm.dqwords.elems[0];
  xmm15->dqwords.elems[0] = state->vec[15].xmm.dqwords.elems[0];
}

// Minimal no-op implementation of `__remill_flat_jump` for single-block
// execution. In the full Saturn runtime, this would dispatch to the next
// basic block. For benchmarking and single-block testing, it simply returns
// the memory pointer unchanged (mirrors `__remill_jump` in saturn_helpers.cpp).
extern "C" Memory *__remill_flat_jump(
    addr_t *pc, Memory *memory, addr_t *next_pc,
    addr_t rax, addr_t rbx, addr_t rcx, addr_t rdx,
    addr_t rsi, addr_t rdi, addr_t rsp, addr_t rbp,
    addr_t r8, addr_t r9, addr_t r10, addr_t r11,
    addr_t r12, addr_t r13, addr_t r14, addr_t r15,
    addr_t rip,
    addr_t ss_base, addr_t gs_base, addr_t cs_base, addr_t fs_base,
    uint8_t cf, uint8_t pf, uint8_t af, uint8_t zf,
    uint8_t sf, uint8_t df, uint8_t of,
    uint64_t mm0, uint64_t mm1, uint64_t mm2, uint64_t mm3,
    uint64_t mm4, uint64_t mm5, uint64_t mm6, uint64_t mm7,
    vec128_t xmm0, vec128_t xmm1, vec128_t xmm2, vec128_t xmm3,
    vec128_t xmm4, vec128_t xmm5, vec128_t xmm6, vec128_t xmm7,
    vec128_t xmm8, vec128_t xmm9, vec128_t xmm10, vec128_t xmm11,
    vec128_t xmm12, vec128_t xmm13, vec128_t xmm14, vec128_t xmm15) {
  (void)pc; (void)next_pc;
  (void)rax; (void)rbx; (void)rcx; (void)rdx;
  (void)rsi; (void)rdi; (void)rsp; (void)rbp;
  (void)r8; (void)r9; (void)r10; (void)r11;
  (void)r12; (void)r13; (void)r14; (void)r15;
  (void)rip;
  (void)ss_base; (void)gs_base; (void)cs_base; (void)fs_base;
  (void)cf; (void)pf; (void)af; (void)zf;
  (void)sf; (void)df; (void)of;
  (void)mm0; (void)mm1; (void)mm2; (void)mm3;
  (void)mm4; (void)mm5; (void)mm6; (void)mm7;
  (void)xmm0; (void)xmm1; (void)xmm2; (void)xmm3;
  (void)xmm4; (void)xmm5; (void)xmm6; (void)xmm7;
  (void)xmm8; (void)xmm9; (void)xmm10; (void)xmm11;
  (void)xmm12; (void)xmm13; (void)xmm14; (void)xmm15;
  return memory;
}

#endif  // 64 == ADDRESS_SIZE_BITS
