#!/usr/bin/env python3
"""remill-ir2c tests.

Each case is an IR file and fragments its C must contain ("!..." for text it
must not contain). Every output must be free of UNSUPPORTED and compile
cleanly: <cc> -std=c99 -Wall -Werror -c.

  run_ir2c.py <remill-ir2c> <cc> <source dir> <work dir>
"""
import os
import subprocess
import sys

CASES = {
    # SICE !LIFT, seeded, stepping through sum_array's loop and back to main
    "sum_array_trace.ll": [
        "*(uint32_t *)(rsp - 8) = 0x4011a6;",   # the call stand-in's return address
        "*(uint64_t *)(rsp - 0x10) = rbp;",     # push rbp
        "*(uint32_t *)0x404040",                # the loop's loads, data[0] ..
        "*(uint32_t *)0x40405c",                # .. data[7]
        "c->rax = 0x402004;",
        "c->rdi = 0x404040;",
        "!c->rcx",                              # seeded and unchanged: not written
        "!pc_init",                             # remill's PC bookkeeping: dropped
    ],
    # SICE !LIFT SYM + MBA: (x ^ y) + 2*(x & y) simplified by SiMBA++
    "mba_add_trace.ll": [
        "= rdi + rsi;",
        "c->rax = ",
        "c->zf = (uint8_t)(",
        "((uint8_t)__builtin_popcountll(",      # PF, with parentheses for & inside ^
        ">> 63",                                # shift counts in decimal
        "!c->rcx",
        "!rip",                                 # only the dropped bookkeeping read it
    ],
    # a plain function: the operations the traces don't cover
    "ops.ll": [
        "uint64_t ops(uint64_t a, uint64_t b, uint8_t c, uint32_t d)",
        "(uint8_t)((uint32_t)c * 3)",           # narrow multiply in 32 bits
        "(int64_t)(int32_t)d >> 3",             # sext + ashr, no round trip
        "(int64_t)a / 7",                       # sdiv, the literal uncast
        "rotl64(a, a, 13)",
        "umin64(a, b)",
        "smax64(a, b)",
        "abs64(b)",
        "__builtin_popcountll(b)",
        "uadd_ov64(a, b)",
        "(a - 8)",                              # add of a negative constant
        "~b",
        "return ",
    ],
}


def main():
    tool, cc, src, work = sys.argv[1:5]
    os.makedirs(work, exist_ok=True)
    failed = 0
    for name, fragments in CASES.items():
        out = os.path.join(work, name.replace(".ll", ".c"))
        r = subprocess.run([tool, "--ir", os.path.join(src, name), "--out", out],
                           capture_output=True, text=True)
        problems = []
        if r.returncode != 0:
            problems.append("remill-ir2c failed: " + r.stderr.strip())
        text = open(out).read() if os.path.exists(out) else ""
        if "UNSUPPORTED" in text:
            problems.append("contains UNSUPPORTED")
        for f in fragments:
            if f.startswith("!"):
                if f[1:] in text:
                    problems.append("must not contain: " + f[1:])
            elif f not in text:
                problems.append("missing: " + f)
        c = subprocess.run([cc, "-std=c99", "-Wall", "-Werror", "-c", out, "-o", out + ".o"],
                           capture_output=True, text=True)
        if c.returncode != 0:
            problems.append("does not compile:\n" + c.stderr.strip())
        if problems:
            failed += 1
            print(f"[FAIL] {name}")
            for p in problems:
                print("       " + p)
            print("------ emitted C ------\n" + text)
        else:
            print(f"[PASS] {name}")
    print("ALL PASSED" if not failed else f"{failed} FAILED")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
