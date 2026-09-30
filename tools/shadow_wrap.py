#!/usr/bin/env python3
"""Copy src/recomp/gen to src/recomp/gen-shadow with every translated function
wrapped for the differential check (src/shadow.c).

    void sub_X(void) { recomp_shadow_call(0xX, sub_X_impl); }
"""
import os, re, shutil, sys

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "src", "recomp", "gen")
dst = os.path.join(here, "..", "src", "recomp", "gen-shadow")
if os.path.isdir(dst):
    shutil.rmtree(dst)
os.makedirs(dst)
pat = re.compile(r"^void (sub_([0-9A-F]{8}))\(void\)\n\{", re.M)
total = 0
for name in sorted(os.listdir(src)):
    s = open(os.path.join(src, name), errors="replace").read()
    if name.startswith("recomp_") and name.endswith(".c") and "dispatch" not in name:
        def rep(m):
            global total
            total += 1
            fn, va = m.group(1), m.group(2)
            return ("void %s_impl(void);\n"
                    "void %s(void) { recomp_shadow_call(0x%s, %s_impl); }\n"
                    "void %s_impl(void)\n{") % (fn, fn, va, fn, fn)
        s = pat.sub(rep, s)
        s = s.replace('#include "recomp_funcs.h"',
                      '#include "recomp_funcs.h"\nvoid recomp_shadow_call(uint32_t va, void (*impl)(void));', 1)
    open(os.path.join(dst, name), "w").write(s)
print("wrapped", total, "functions ->", os.path.normpath(dst))
