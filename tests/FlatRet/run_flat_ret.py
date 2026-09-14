#!/usr/bin/env python3
"""End-to-end execution regression test for flat-lifted function results.

The flat lift of a whole function is a single instruction-execution loop that
takes the 63-arg flat register file and ends with a tail call to
``__remill_flat_jump`` carrying the final register file. A function's ``ret``
result is therefore observable as the RAX argument of that dispatch.

This test lifts two fixed byte blobs, links the lifted IR against a small C
harness that (a) stubs ``__remill_flat_jump`` to capture the GPRs and
(b) drives the lifted function with concrete inputs, then checks the captured
RAX against a reference implementation.

It specifically guards two bugs that made the ``ret`` result unobservable:
  1. The register-destination (dst) store being deleted by a position-based
     snapshot-clobber removal (lost the computed RAX).
  2. ``__remill_compare_neq`` being inlined as ``!x`` (inverting every JNZ).

Usage:
    run_flat_ret.py <lift_binary> <clang> <fixture_dir> <work_dir>
"""
import os
import subprocess
import sys
import textwrap

# 63-arg flat register-file ABI (matches the lifted `define`).
HARNESS_HEAD = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <immintrin.h>

unsigned char __remill_undefined_8(void) { return 0; }

static unsigned long long g_RAX;
void *__remill_flat_jump(void *PC, void *mem, void *NEXT_PC,
                         unsigned long long RAX, unsigned long long RBX,
                         unsigned long long RCX, unsigned long long RDX,
                         unsigned long long RSI, unsigned long long RDI,
                         ...) {
  g_RAX = RAX;
  return (void *)0;
}

extern void *%(name)s(
    void *PC, void *memory, void *NEXT_PC,
    unsigned long long RAX, unsigned long long RBX,
    unsigned long long RCX, unsigned long long RDX,
    unsigned long long RSI, unsigned long long RDI,
    void *RSP, void *RBP,
    unsigned long long R8,  unsigned long long R9,
    unsigned long long R10, unsigned long long R11,
    unsigned long long R12, unsigned long long R13,
    unsigned long long R14, unsigned long long R15,
    unsigned long long RIP, unsigned long long SS_BASE,
    unsigned long long GS_BASE, unsigned long long CS_BASE,
    unsigned long long FS_BASE,
    unsigned char CF, unsigned char PF, unsigned char AF,
    unsigned char ZF, unsigned char SF, unsigned char DF, unsigned char OF,
    unsigned long long MM0, unsigned long long MM1,
    unsigned long long MM2, unsigned long long MM3,
    unsigned long long MM4, unsigned long long MM5,
    unsigned long long MM6, unsigned long long MM7,
    __m128i XMM0,  __m128i XMM1,  __m128i XMM2,  __m128i XMM3,
    __m128i XMM4,  __m128i XMM5,  __m128i XMM6,  __m128i XMM7,
    __m128i XMM8,  __m128i XMM9,  __m128i XMM10, __m128i XMM11,
    __m128i XMM12, __m128i XMM13, __m128i XMM14, __m128i XMM15,
    unsigned __int128 ST0, unsigned __int128 ST1,
    unsigned __int128 ST2, unsigned __int128 ST3,
    unsigned __int128 ST4, unsigned __int128 ST5,
    unsigned __int128 ST6, unsigned __int128 ST7);

long reference(long a, long b);

static long run_lifted(long a, long b) {
  static unsigned char buf[512];
  unsigned long long pc = %(entry)d, npc = %(entry)d;
  static unsigned long long membuf[64];
  void *rsp = (void *)(buf + 480);
  void *rbp = (void *)(buf + 480);
  __m128i z = _mm_setzero_si128();
  void *r = %(name)s(
      &pc, membuf, &npc,
      0, 0, 0, 0,
      (unsigned long long)b, (unsigned long long)a,
      rsp, rbp,
      0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0,
      z, z, z, z, z, z, z, z, z, z, z, z, z, z, z, z,
      0, 0, 0, 0, 0, 0, 0, 0);
  (void)r;
  return (long)g_RAX;
}

int main(int argc, char **argv) {
  long a = argc > 1 ? atol(argv[1]) : 0;
  long b = argc > 2 ? atol(argv[2]) : 0;
  long got = run_lifted(a, b);
  long want = reference(a, b);
  printf("RAX=%%ld WANT=%%ld %%s\n", got, want, got == want ? "PASS" : "FAIL");
  return got == want ? 0 : 1;
}
"""

# Reference implementations + the (a,b) cases per test, and lift parameters.
CASES = [
    {
        "tag": "op1",
        "bin": "op1.bin",
        "name": "sub_1170",
        "base": "0x1050",
        "entry": "0x1170",
        "reference": (
            "long reference(long a, long b) {\n"
            "  long t = (a ^ b) + 2 * (a & b);   /* (a^b)+2(a&b) */\n"
            "  if (a + b == t) return t;          /* equal: no imul */\n"
            "  return 3 * t;                      /* jne taken: imul 3 */\n"
            "}\n"
        ),
        # (a, b, expected)
        "cases": [
            (3, 7, 10),
            (5, 5, 10),
            (1, 2, 3),
            (10, 10, 20),
            (0, 0, 0),
            (4, 8, 12),
        ],
    },
    {
        "tag": "br3",
        "bin": "br3.bin",
        "name": "sub_1000",
        "base": "0x1000",
        "entry": "0x1000",
        "reference": (
            "long reference(long a, long b) {\n"
            "  long x = a;                 /* RDI */\n"
            "  (void)b;\n"
            "  if (x == 7) return x * 2;   /* jne not taken */\n"
            "  return x + 10;              /* jne taken */\n"
            "}\n"
        ),
        # (a, b, expected)
        "cases": [
            (7, 0, 14),
            (5, 0, 15),
            (0, 0, 10),
            (9, 0, 19),
            (100, 0, 110),
        ],
    },
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    if len(sys.argv) < 5:
        sys.stderr.write(
            "usage: run_flat_ret.py <lift> <clang> <fixture_dir> <work_dir>\n")
        return 2
    lift, clang, fx, work = sys.argv[1:5]
    os.makedirs(work, exist_ok=True)
    failures = 0

    for c in CASES:
        with open(os.path.join(fx, c["bin"]), "rb") as f:
            blob = f.read()
        xxd = blob.hex()
        ir = os.path.join(work, c["tag"] + ".ll")
        r = run([lift, "--arch", "amd64", "--address", c["base"],
                 "--entry_address", c["entry"], "--flat",
                 "--bytes", xxd, "--ir_out", ir])
        if r.returncode != 0 or not os.path.exists(ir):
            sys.stderr.write(f"[{c['tag']}] LIFT FAILED:\n{r.stderr}\n")
            failures += 1
            continue
        sys.stdout.write(f"[{c['tag']}] lifted -> {ir}\n")

        src = HARNESS_HEAD % {"name": c["name"], "entry": int(c["entry"], 16)}
        src += c["reference"]
        harness = os.path.join(work, c["tag"] + "_harness.c")
        with open(harness, "w") as f:
            f.write(src)
        exe = os.path.join(work, c["tag"] + "_test")
        b = run([clang, ir, harness, "-O2", "-o", exe])
        if b.returncode != 0:
            sys.stderr.write(f"[{c['tag']}] LINK FAILED:\n{b.stderr}\n")
            failures += 1
            continue

        for (a, b, _want) in c["cases"]:
            t = run([exe, str(a), str(b)])
            out = t.stdout.strip()
            ok = t.returncode == 0 and "PASS" in out
            sys.stdout.write(
                f"  [{'PASS' if ok else 'FAIL'}] {c['tag']}({a},{b}): {out}\n")
            if not ok:
                failures += 1

    if failures:
        sys.stdout.write(f"\n{failures} FAILURE(S)\n")
        return 1
    sys.stdout.write("\nALL PASS\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
