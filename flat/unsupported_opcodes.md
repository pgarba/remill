# Opcodes Not Supported in Flat Mode

## Flat ABI Coverage (51 registers)

| Category | Registers | Count |
|----------|-----------|-------|
| GPR | rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp, r8-r15, rip | 17 |
| Segment bases | ss_base, gs_base, cs_base | 3 |
| Flags | cf, pf, af, zf, sf, df, of | 7 |
| MMX | mm0-mm7 | 8 |
| XMM | xmm0-xmm15 (vec[0-15]) | 16 |
| **Total** | | **51** |

## NOT Covered (Unsupported in Flat Mode)

### AVX512 Wide Registers (vec[16-31]: ymm16-31, zmm0-31)

**294 functions, 104 unique opcodes**

These opcodes use 256-bit (ymm) or 512-bit (zmm) vector registers.
The flat ABI only has 16 XMM slots (128-bit). Extending to 32 vec slots
would add 16 more pointer arguments.

```
ADDPD
ADDPS
ADDSD
ADDSS
CMPPD
CMPPS
CMPSD
CMPSS
CVTDQ2PD
CVTDQ2PS
CVTPD2DQ
CVTPD2PS
CVTPS2DQ
CVTPS2PD
CVTSS2SD
DIVPD
DIVPS
DIVSD
DIVSS
HADDPD
HADDPS
MAXSD
MAXSS
MINSD
MINSS
MOV
MOVD
MOVDDUP
MOVDQ
MOVQ
MOVSD_MEM
MOVSS_MEM
MULPD
MULPS
MULSD
MULSS
PACKUSWB
PACKUSWB_AVX
PADDQ
PAND
PANDN
PANDN_64
PAND_64
PCMPEQB
PCMPEQD
PCMPEQQ
PCMPEQW
PCMPGTB
PCMPGTD
PCMPGTQ
PCMPGTW
PINSRW
PMOVMSKB
POR
POR_64
PSADBW
PSHUFD
PSHUFLW
PSLLDQ
PSLLQ
PSLLQ_V
PSRLDQ
PSRLQ
PSRLQ_V
PSUBD
PSUBQ
PTEST
PXOR
PXOR_64
SUBPD
SUBPS
SUBSD
SUBSS
UNPCKHPD
UNPCKHPS
UNPCKLPD
UNPCKLPS
VCVTSD2SS
VCVTSI2SD
VCVTSI2SS
VCVTSS2SD
VFMADD132SD
VFMADD213SD
VFMADD231SD
VFMSUB132SD
VFMSUB213SD
VFMSUB231SD
VINSERTF128
VMOVHLPSEP6M
VMOVHPDEP6M
VMOVHPSEP6M
VMOVLHPSEP6M
VMOVLPDEP6M
VMOVLPSEP6M
VMOVSDEP6M
VMOVSSEP6M
VPBROADCASTB
VPMOVSXBQ_MASK
VPMOVSXWD_MASK
VPSLLDQ
VPSRLDQ
VRSQRTSS
VSQRTSD
VSQRTSS
```

### X87 FPU Stack (st0-st7, 80-bit extended precision)

**101 functions, 33 unique opcodes**

These opcodes use the x87 FPU register stack (80-bit floats).
The flat ABI has no x87 slots. Would need 8 x float80_t pointers.

```
D
F
FADD
FADDP
FCOM
FCOMP
FD
FDIV
FDIVP
FDIVR
FDIVRP
FFREEEP6M
FFREEPEP6M
FILD
FLD
FLDCWEP6M
FMUL
FMULP
FNSTCWEP6M
FNSTSW
FST
FSTP
FSUB
FSUBP
FSUBR
FSUBRP
FUCOM
FUCOMP
FXCHEP6M
PFADD
PFMUL
PFSUB
PFSUBR
```

### K_REG Mask Registers (k0-k7, AVX512 opmask)

**4 functions, 2 unique opcodes**

These opcodes use AVX512 mask registers (64-bit opmasks).
The flat ABI has no K_REG slots. Would need 8 x uint64_t pointers.

```
VPMOVSXBQ_MASK
VPMOVSXWD_MASK
```

## Summary

| Category | Functions | Unique Opcodes | Fix |
|----------|-----------|----------------|-----|
| AVX512 wide (vec[16-31]) | 294 | 104 | Extend flat ABI: +16 vec slots |
| X87 FPU (st0-st7) | 101 | 33 | Extend flat ABI: +8 float80 slots |
| K_REG masks (k0-k7) | 4 | 2 | Extend flat ABI: +8 uint64 slots |
| **Total unsupported** | **399** | | |

## Base amd64 (1589 opcodes)

**All 1589 base amd64 opcodes are supported in flat mode.**
They only use GPRs, XMM (vec[0-15]), MMX, flags, and segment bases —
all covered by the 51-register flat ABI.
