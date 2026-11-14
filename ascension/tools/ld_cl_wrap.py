#!/usr/bin/env python3
"""
ld_cl_wrap.py -- phase 1 of the D3D12 recorder (my-changes\\recorder\\DESIGN.md):
route every command-list call in rsx_live_draw.c through one macro, with no
change to the code it compiles to.

    g.list->lpVtbl->Method(g.list, a, b)   ->   LD_CL(Method, a, b)
    g.list->lpVtbl->Close(g.list)          ->   LD_CL0(Close)

and, once, after the includes:

    #define LD_CL(M, ...) g.list->lpVtbl->M(g.list, __VA_ARGS__)
    #define LD_CL0(M) g.list->lpVtbl->M(g.list)

which expand to the same tokens, so the object code is the same (check it:
compile the file before and after from the same path and compare the .obj).
The call is matched with a parenthesis scanner (strings, comments and nested
calls skipped), so calls spanning several lines are handled. Left alone:
QueryInterface and SetName (phase 2 drains the recorder before them) and any
other list object (list1, ...). Every site is listed; the diff is the review.

Usage:
  python tools/ld_cl_wrap.py <in rsx_live_draw.c> <out file> [--check]
  --check: only count the sites, write nothing.
"""
import re, sys

SKIP = {"QueryInterface", "SetName"}
HEAD = re.compile(r"g\.list->lpVtbl->(\w+)\(")
MACROS = ("/* D3D12 recorder, phase 1 (tools/ld_cl_wrap.py): every command-list call\n"
          " * goes through LD_CL. Today it expands to the direct call, token for token. */\n"
          "#define LD_CL(M, ...) g.list->lpVtbl->M(g.list, __VA_ARGS__)\n"
          "#define LD_CL0(M) g.list->lpVtbl->M(g.list)\n")


def skip_ws_comments(s, i):
    while i < len(s):
        if s[i].isspace():
            i += 1
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            i = len(s) if j < 0 else j + 2
        elif s.startswith("//", i):
            j = s.find("\n", i)
            i = len(s) if j < 0 else j
        else:
            break
    return i


def match_paren(s, i):
    """s[i] == '(' -> index of its ')' (strings, chars and comments skipped)."""
    depth = 0
    while i < len(s):
        c = s[i]
        if c in "\"'":
            q = c; i += 1
            while i < len(s) and s[i] != q:
                i += 2 if s[i] == "\\" else 1
        elif s.startswith("/*", i):
            i = s.find("*/", i + 2) + 1
        elif s.startswith("//", i):
            i = s.find("\n", i)
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced parenthesis")


def wrap(src):
    out, pos, sites, left = [], 0, [], []
    for m in HEAD.finditer(src):
        if m.start() < pos:
            continue
        method = m.group(1)
        line = src.count("\n", 0, m.start()) + 1
        if method in SKIP:
            left.append((line, method))
            continue
        open_i = m.end() - 1
        close_i = match_paren(src, open_i)
        a = skip_ws_comments(src, open_i + 1)
        if not src.startswith("g.list", a):
            left.append((line, method + " (first argument is not g.list)"))
            continue
        b = skip_ws_comments(src, a + len("g.list"))
        if b == close_i:
            new = f"LD_CL0({method})"
        elif src[b] == ",":
            rest = src[b + 1:close_i]
            # keep the argument text as written; the comma after g.list goes
            new = f"LD_CL({method},{rest})"
        else:
            left.append((line, method + " (unexpected text after g.list)"))
            continue
        out.append(src[pos:m.start()])
        out.append(new)
        pos = close_i + 1
        sites.append((line, method, src.count("\n", m.start(), close_i) + 1))
    out.append(src[pos:])
    return "".join(out), sites, left


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    raw = open(sys.argv[1], "rb").read()
    crlf = b"\r\n" in raw
    src = raw.decode("utf-8").replace("\r\n", "\n")
    if "#define LD_CL(" in src:
        sys.exit("already wrapped (LD_CL is defined)")
    new, sites, left = wrap(src)
    for line, method, nlines in sites:
        print(f"  {line:6}: {method}" + (f"  ({nlines} lines)" if nlines > 1 else ""))
    for line, why in left:
        print(f"  {line:6}: LEFT {why}")
    print(f"{len(sites)} sites wrapped ({sum(1 for s in sites if s[2] > 1)} multi-line), {len(left)} left")
    if "--check" in sys.argv:
        return
    # The macros go after the last #include of the opening block (before any code).
    m = None
    for m in re.finditer(r"(?m)^#include [<\"][^>\"]+[>\"][^\n]*\n", new[:20000]):
        pass
    at = m.end() if m else 0
    new = new[:at] + MACROS + new[at:]
    out = new.replace("\n", "\r\n") if crlf else new
    open(sys.argv[2], "wb").write(out.encode("utf-8"))


if __name__ == "__main__":
    main()
