#!/usr/bin/env python3
"""remill-ir2c tests.

Each case is an IR file and fragments its C must contain ("!..." for text it
must not contain). Every output must be free of UNSUPPORTED and compile
cleanly: <cc> -std=c99 -Wall -Werror -c.

  run_ir2c.py <remill-ir2c> <cc> <source dir> <work dir>
"""
import os
import re
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
        "!c->rcx =",                            # seeded and unchanged: not written
        "!pc_init",                             # remill's PC bookkeeping: dropped
    ],
    # SICE !LIFT SYM + MBA: (x ^ y) + 2*(x & y) simplified by SiMBA++
    "mba_add_trace.ll": [
        "= rdi + rsi;",
        "c->rax = ",
        "c->zf = (uint8_t)(",
        "((uint8_t)__builtin_popcountll(",      # PF, with parentheses for & inside ^
        ">> 63",                                # shift counts in decimal
        "!c->rcx =",
        "!rip",                                 # only the dropped bookkeeping read it
    ],
    # a symbolic rep stosq (one-shot): remill lifts the rep as a loop
    "rep_stosq_loop.ll": [
        "goto bb",                              # the loop's back edge
        "*(uint64_t *)",                        # the store
        "if (rcx == 0)",                        # rcx = 0: no iteration
        "c->rcx = 0;",
    ],
    # lift mode stepping rep movsb: each iteration as plain movsb -- linear
    "rep_movsb_trace.ll": [
        "*(uint8_t *)0x404060 = *(uint8_t *)0x404020;",
        "*(uint8_t *)0x404063 = *(uint8_t *)0x404023;",
        "c->rcx = 0;",
        "!goto",                                # no loop left
    ],
    # div: SICE defines __remill_error as a trap (#DE), so remill's State
    # buffer is gone and the checks read as traps
    "div_trap.ll": [
        "__builtin_trap();",
        "((unsigned __int128)rdx << 64) | (unsigned __int128)rax",
        "c->rax = (uint64_t)",
        "!c->rbx =",                            # freeze(rbx): unchanged
        "!alloca",
    ],
    # lock cmpxchg: the C11 builtin, the fences, and ZF from its success
    "lock_cmpxchg.ll": [
        "__atomic_compare_exchange_n((uint32_t *)rdi, &",
        "__atomic_thread_fence(__ATOMIC_SEQ_CST);",
        "c->zf = ",
    ],
    # shl rax, cl: a count of 0 leaves the flags (a switch on the count)
    "shl_cl.ll": [
        "switch (",
        "case 0:",
        "goto bb",
    ],
    # SICE !LIFT stepping from the exe through the PLT into the loader's
    # resolver: two segments (the second out of RIP-relative reach of the
    # first) stitched into one function; opt merged four stores into a memset
    "stitched_trace.ll": [
        "*(uint32_t *)(rsp - 8) = 0x4011be;",   # the call stand-in
        "*(uint64_t *)0x7ffff7ffcc90",          # sub rsp, [rip+..] in the 2nd segment
        "__builtin_memset((void *)(rsp_next_i + 0x250), 0, 0x20);",
        "c->rsp = rsp_next_i;",
        "!sice_seg",                            # inlined: no call left
    ],
    # floating point and XMM vectors
    "fp.ll:fparith": ["(double)(int64_t)c", "a - 2.5", "-", "a < b ?"],
    "fp.ll:fcmps": ["__builtin_islessgreater(a, b)", "__builtin_isunordered(a, b)", "(uint32_t)!(a <= b)"],
    "fp.ll:conv": ["(uint32_t)(int32_t)a", "(double)f", "(float)a", "__builtin_sqrt(", "__builtin_floorf(",
                   "__builtin_fmax(", "bits_f64(", "bits_f32("],
    "fp.ll:sse": ["f64_bits(", "bits_f64(", "unsigned __int128", ">> 96", "!UNSUPPORTED"],
    "mem_intrinsics.ll": [
        "__builtin_memset((void *)d, 0, 0x20);",
        "__builtin_memcpy((void *)(d + 0x40), (const void *)s, 0x10);",
        "__builtin_memmove((void *)d, (const void *)s, n);",
        "!noalias",
    ],
    # branches in plain functions (also run against the IR, below)
    "branchy.ll:loop_sum": ["goto bb", "!UNSUPPORTED"],
    "branchy.ll:swap": ["{ uint64_t phi_0 = "],  # the parallel copy
    "branchy.ll:sw": ["switch (", "case 0:", "default:"],
    "branchy.ll:mulhi": ["unsigned __int128", ">> 64"],
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
        file, _, fn = name.partition(":")
        out = os.path.join(work, (file.replace(".ll", "") + ("_" + fn if fn else "")) + ".c")
        cmd = [tool, "--ir", os.path.join(src, file), "--out", out] + (["--function", fn] if fn else [])
        r = subprocess.run(cmd, capture_output=True, text=True)
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
    failed += differential(tool, cc, src, work)
    print("ALL PASSED" if not failed else f"{failed} FAILED")
    return 1 if failed else 0


# Plain functions run both ways: the emitted C, and the IR itself (compiled
# with its functions renamed *_ir), on the same random inputs.
DIFF = {"ops.ll": ["ops"], "branchy.ll": ["loop_sum", "diamond", "swap", "mulhi", "sw"],
        "fp.ll": ["fparith", "fcmps", "conv", "sse"]}
CTYPE = {"i1": "bool", "i8": "uint8_t", "i16": "uint16_t", "i32": "uint32_t", "i64": "uint64_t",
         "double": "double", "float": "float"}
# FP inputs: eighths in -250..250 (the range the conversions are defined
# for), and now and then a NaN for the unordered compares (conv is defined for
# finite inputs only); results compare bit for bit (the default NaN both ways)
FP_DRAW = "({t})((double)((int64_t)(rnd() % 4001) - 2000) / 8.0)"
NAN_FNS = {"fcmps", "fparith", "sse"}


def differential(tool, cc, src, work):
    failed = 0
    for file, fns in DIFF.items():
        ir = open(os.path.join(src, file)).read()
        renamed = re.sub(r"@(" + "|".join(re.findall(r"^define [^@]*@([\w.]+)\(", ir, re.M)) + r")\(",
                         r"@\1_ir(", ir)
        ir_path = os.path.join(work, file.replace(".ll", "_ir.ll"))
        open(ir_path, "w").write(renamed)
        ir_obj = ir_path + ".o"
        r = subprocess.run([cc, "-O1", "-c", ir_path, "-o", ir_obj], capture_output=True, text=True)
        if r.returncode:
            print(f"[FAIL] {file}: the IR does not compile:\n{r.stderr}")
            failed += 1
            continue
        for fn in fns:
            m = re.search(r"^define (\w+) @" + re.escape(fn) + r"\(([^)]*)\)", ir, re.M)
            ret, params = CTYPE[m.group(1)], [CTYPE[p.split()[0]] for p in m.group(2).split(",")]
            c_path = os.path.join(work, f"diff_{fn}.c")
            subprocess.run([tool, "--ir", os.path.join(src, file), "--function", fn, "--out", c_path], check=True)
            args = ", ".join(f"a{k}" for k in range(len(params)))
            decl = ", ".join(params)
            fp = FP_DRAW if fn not in NAN_FNS else "(i % 11 == 3 ? __builtin_nan(\"\") : " + FP_DRAW + ")"
            draws = "\n".join(f"        {t} a{k} = " + (fp.format(t=t) if t in ("double", "float")
                                                    else f"({t})(i % 5 ? rnd() : rnd() % 4)") + ";"
                               for k, t in enumerate(params))
            driver = f"""#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
{ret} {fn}({decl});
{ret} {fn}_ir({decl});
static uint64_t s = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {{ s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }}
int main(void) {{
    for (int i = 0; i < 20000; ++i) {{
{draws}
        {ret} r = {fn}({args}), r_ir = {fn}_ir({args});
        if (memcmp(&r, &r_ir, sizeof r)) {{
            printf("mismatch at input %d\\n", i);
            return 1;
        }}
    }}
    return 0;
}}
"""
            drv = os.path.join(work, f"diff_{fn}_main.c")
            open(drv, "w").write(driver)
            exe = os.path.join(work, f"diff_{fn}")
            b = subprocess.run([cc, "-std=c99", "-O1", "-ffp-contract=off", drv, c_path, ir_obj, "-lm", "-o", exe],
                               capture_output=True, text=True)
            run = subprocess.run([exe], capture_output=True, text=True) if b.returncode == 0 else None
            if b.returncode or run.returncode:
                failed += 1
                print(f"[FAIL] {file}:{fn} (C vs IR): " + (b.stderr.strip() if b.returncode else run.stdout.strip()))
            else:
                print(f"[PASS] {file}:{fn} (C vs IR, 20000 random inputs)")
    return failed


if __name__ == "__main__":
    sys.exit(main())
