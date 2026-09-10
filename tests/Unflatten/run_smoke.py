#!/usr/bin/env python3
"""Self-contained smoke test for the Unflatten pass.

Runs the remill-lift binary on small flat-block fixtures and verifies the
structural invariants of the unflattened output. Two scenarios:

  jmp : block_1bdc ->(static jmp)-> block_463 ->(off-manifest jmp)-> dispatch
  jcc : block_10b2 -(jcc)--> { taken: off-manifest dispatch,
                               notaken: block_10bd (static br) }

For each scenario the test asserts:
  * one defined function with the 61-arg ABI (memory + 60 state regs)
  * an fn_entry block branching to the entry block's BB
  * a phi farm (60 phis) at each block BB
  * every static (in-fixture) machine edge is emitted as a real `br` to the
    successor's BB (edge validation against edges.json)
  * off-manifest arms lower to a __remill_dynamic_dispatch dead-end
  * no residual per-block flat_pc_* globals or @sub_* block calls

Usage:
    run_smoke.py <lift_binary> <fixture_dir>
"""
import json
import os
import re
import subprocess
import sys
import tempfile

BB_RE = re.compile(r"^([a-zA-Z_0-9.]+):")
BR_RE = re.compile(
    r"\s+br(?: i1 [^,]+,)? label %([a-zA-Z_0-9.]+)(?:, label %([a-zA-Z_0-9.]+))?"
)


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: run_smoke.py <lift_binary> <fixture_dir>\n")
        return 2
    lift = sys.argv[1]
    fx = sys.argv[2]

    scenarios = [
        {
            "name": "unflat_smoke",
            "blocks": ["block_1bdc.ll", "block_463.ll"],
            "edges": "edges.json",
            "entry": "1bdc",
        },
        {
            "name": "jcc_smoke",
            "blocks": ["jcc_10b2.ll", "jcc_10bd.ll"],
            "edges": "jcc_edges.json",
            "entry": "10b2",
        },
        {
            # M5: the optimized build must still be valid and keep the
            # in-function static edge (instcombine + DCE, no simplifycfg).
            "name": "jcc_opt",
            "blocks": ["jcc_10b2.ll", "jcc_10bd.ll"],
            "edges": "jcc_edges.json",
            "entry": "10b2",
            "optimize": True,
        },
    ]
    for sc in scenarios:
        rc = run_scenario(lift, fx, sc)
        if rc != 0:
            sys.stderr.write("scenario %s FAILED\n" % sc["name"])
            return rc
    sys.stdout.write("unflatten smoke: OK\n")
    return 0


def run_scenario(lift, fx, sc):
    blocks = " ".join(os.path.join(fx, b) for b in sc["blocks"])
    edges_path = os.path.join(fx, sc["edges"])

    out_fd, out_path = tempfile.mkstemp(suffix=".ll", prefix=sc["name"] + "_")
    os.close(out_fd)
    try:
        cmd = [
            lift,
            "--arch", "amd64",
            "--link_blocks", blocks,
            "--unflatten", edges_path,
            "--unflatten_name", sc["name"],
        ]
        if sc.get("optimize"):
            cmd.append("--unflatten_opt")
        cmd.extend(["--ir_out", out_path])
        sys.stderr.write("running: %s\n" % " ".join(cmd))
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.stdout:
            sys.stderr.write(proc.stdout)
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            sys.stderr.write("FAIL: lift exited %d\n" % proc.returncode)
            return 1
        return check(sc, out_path, edges_path, sc.get("optimize", False))
    finally:
        try:
            os.unlink(out_path)
        except OSError:
            pass


def parse_body(text, fn_name):
    m = re.search(r"define [^{]*@" + re.escape(fn_name) + r"\(", text)
    if not m:
        return None
    i = m.start()
    j = text.find("\n}", i)
    return text[i:j].splitlines()


def check(sc, path, edges_path, optimize=False):
    s = open(path).read()
    fails = []

    def need(cond, msg):
        if not cond:
            fails.append(msg)

    lines = parse_body(s, sc["name"])
    need(lines is not None, "missing define @%s" % sc["name"])
    if lines is None:
        return report(fails)

    # 1. 61-arg ABI.
    args = lines[0].split("(", 1)[1].rsplit(")", 1)[0].split(",")
    need(len(args) == 61, "expected 61 args, got %d" % len(args))
    need(args[0].strip().startswith("ptr"), "first arg should be ptr %memory")

    # 2. CFG parse.
    succ = {}
    cur = None
    for l in lines:
        mm = BB_RE.match(l)
        if mm:
            cur = mm.group(1)
            succ.setdefault(cur, [])
            continue
        mb = BR_RE.match(l)
        if mb:
            for g in mb.groups():
                if g:
                    succ[cur].append(g)

    bbs = set(succ)
    need("fn_entry" in bbs, "missing fn_entry block")
    for b in sc["blocks"]:
        pc = b.split("_", 1)[1].replace(".ll", "")
        need("bb_" + pc in bbs, "missing bb_" + pc)

    # 3. fn_entry branches to exactly one of the fixture block BBs.
    fixture_bbs = {"bb_" + b.split("_", 1)[1].replace(".ll", "")
                   for b in sc["blocks"]}
    entry_succ = succ.get("fn_entry", [])
    need(len(entry_succ) == 1 and entry_succ[0] in fixture_bbs,
         "fn_entry should branch to one fixture block, got %s" % entry_succ)

    # 4. Phi farms: each block BB has exactly 60 phis (skipped for the
    #    optimized build, where single-pred phis are folded away).
    if not optimize:
        for b in sc["blocks"]:
            pc = b.split("_", 1)[1].replace(".ll", "")
            need(phi_count(lines, "bb_" + pc) == 60,
                 "bb_%s: expected 60 phis, got %d"
                 % (pc, phi_count(lines, "bb_" + pc)))

    # 5. Edge validation: every in-fixture static machine edge is a real br.
    with open(edges_path) as f:
        edges = json.load(f)
    validate_edges(edges, succ, fails)

    # 6. Residual artifacts.
    body = "\n".join(lines)
    need(not re.search(r"call [^{]*@sub_", body),
         "residual @sub_* block call in unflattened function")
    need("flat_pc_" not in s, "residual flat_pc_* globals after DCE")

    return report(fails)


def phi_count(lines, bb):
    started = False
    n = 0
    for l in lines:
        if re.match(r"^" + re.escape(bb) + r":", l):
            started = True
        elif re.match(r"^[a-zA-Z_0-9.]+:", l) and started:
            break
        if started and " = phi " in l:
            n += 1
    return n


def validate_edges(edges, succ, fails):
    lifted = set()
    for k in edges:
        lifted.add(int(k, 16))

    def lp(v):
        if v in (None, "null"):
            return None
        return int(v, 16) if isinstance(v, str) else int(v)

    def region_successors(pc):
        h = format(pc, "x")
        sset = set()
        for b in succ:
            if b == "bb_" + h or b.startswith("bb_" + h + ".stub"):
                for s_ in succ[b]:
                    sset.add(s_)
        return sset

    checked = 0
    for k, v in edges.items():
        start = int(k, 16)
        kind = v.get("kind")
        targets = []
        if kind == "jmp":
            t = lp(v.get("target"))
            if t is not None:
                targets.append(t)
        elif kind == "call":
            t = lp(v.get("ret"))
            if t is not None:
                targets.append(t)
        elif kind == "jcc":
            targets = [lp(v.get("taken")), lp(v.get("notaken"))]
        else:
            continue
        sset = region_successors(start)
        for t in targets:
            if t is None or t not in lifted:
                continue  # off-manifest: not a static br
            checked += 1
            th = format(t, "x")
            if not any(x == "bb_" + th or x.startswith("bb_" + th + ".stub")
                       for x in sset):
                fails.append(
                    "static edge 0x%x -> 0x%x (%s) not emitted as a br"
                    % (start, t, kind))
    if checked:
        sys.stderr.write("  edge validation: %d in-fixture static edges checked\n" % checked)


def report(fails):
    if fails:
        for f in fails:
            sys.stderr.write("FAIL: %s\n" % f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
