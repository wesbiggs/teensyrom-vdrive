"""A small BASIC V2 interpreter, just enough to run util/vdconfig.bas on the host: the statements and functions that program uses, a 40x25 text
screen (cursor control, reverse, colour codes), scripted keys, and file I/O through a drive object. It is a test aid, not a faithful C64:
strings hold PETSCII codes (lower case in the source = unshifted PETSCII, i.e. upper case letters), numbers are Python numbers.
Differences that matter are modelled on purpose: FOR/NEXT always runs the body once, GET# of a 0 byte gives "", RETURN drops the FORs above its GOSUB."""
import re

COLOR_OF = {144: 0, 5: 1, 28: 2, 159: 3, 156: 4, 30: 5, 31: 6, 158: 7, 129: 8, 149: 9, 150: 10, 151: 11, 152: 12, 153: 13, 154: 14, 155: 15}


class NeedKey(Exception): pass
class Stopped(Exception): pass
class BasicError(Exception): pass


class Screen:
    def __init__(self):
        self.ch = [[" "] * 40 for _ in range(25)]
        self.rev = [[False] * 40 for _ in range(25)]
        self.col = [[1] * 40 for _ in range(25)]
        self.r = self.c = 0
        self.reverse = False
        self.color = 1
        self.scrolled = 0

    def clear(self):
        for r in range(25):
            for c in range(40): self.ch[r][c] = " "; self.rev[r][c] = False; self.col[r][c] = self.color
        self.r = self.c = 0

    def put(self, code):
        if code == 147: self.clear(); return
        if code == 19: self.r = self.c = 0; return
        if code == 18: self.reverse = True; return
        if code == 146: self.reverse = False; return
        if code in COLOR_OF: self.color = COLOR_OF[code]; return
        if code == 17: self.r = min(24, self.r + 1); return
        if code == 145: self.r = max(0, self.r - 1); return
        if code == 13: self.c = 0; self.newline(); self.reverse = False; return
        if code < 32: return
        self.ch[self.r][self.c] = chr(code); self.rev[self.r][self.c] = self.reverse; self.col[self.r][self.c] = self.color
        self.c += 1
        if self.c >= 40: self.c = 0; self.newline()    # the C64 wraps as soon as the last column is written (and scrolls on the last row)

    def newline(self):
        if self.r < 24: self.r += 1
        else:
            self.scrolled += 1
            self.ch = self.ch[1:] + [[" "] * 40]; self.rev = self.rev[1:] + [[False] * 40]; self.col = self.col[1:] + [[1] * 40]

    def row(self, r): return "".join(self.ch[r]).rstrip()
    def text(self): return "\n".join(self.row(r) for r in range(25))


def split_top(s, sep):
    out, cur, q = [], "", False
    for ch in s:
        if ch == '"': q = not q
        if ch == sep and not q: out.append(cur); cur = ""
        else: cur += ch
    out.append(cur)
    return out


def find_top(s, word):
    q = False
    for i in range(len(s)):
        if s[i] == '"': q = not q
        if not q and s.startswith(word, i): return i
    return -1


KEYWORDS = ["print#", "input#", "get#", "gosub", "goto", "return", "rem", "if", "for", "next", "end", "dim", "read", "data", "get", "open", "close", "print", "poke", "sys"]


class Basic:
    def __init__(self, src, io, mem=None):
        self.io = io
        self.screen = Screen()
        self.mem = dict(mem or {})
        self.keys = []
        self.vars, self.arrays = {}, {}
        self.lines = []
        self.index = {}
        self.data, self.dp = [], 0
        for raw in src.splitlines():
            m = re.match(r"\s*(\d+)\s*(.*)$", raw)
            if not m: continue
            num, rest = int(m.group(1)), m.group(2)
            stmts = [] if rest.lower().startswith("rem") else [s for s in split_top(rest, ":")]
            if rest.lower().startswith("data"): stmts = [rest]
            self.index[num] = len(self.lines)
            self.lines.append((num, stmts))
            for s in stmts:
                if s.lstrip().lower().startswith("data"):
                    for v in split_top(s.lstrip()[4:], ","): self.data.append(float(v) if re.match(r"^\s*-?\d+(\.\d*)?\s*$", v) else v.strip().strip('"'))
        self.li, self.si = 0, 0
        self.gosubs, self.fors = [], []
        self.ended = False
        self.steps = 0

    # ---- helpers ----
    @staticmethod
    def vname(n):
        n = n.lower(); s = n.endswith("$")
        return n.rstrip("$")[:2] + ("$" if s else "")

    def getvar(self, n):
        n = self.vname(n)
        if n == "st": return self.io.st
        return self.vars.get(n, "" if n.endswith("$") else 0)

    def setvar(self, n, v):
        n = self.vname(n)
        if n.endswith("$") != isinstance(v, str): raise BasicError("type mismatch %s" % n)
        self.vars[n] = v

    def key(self, name, idx):
        return (self.vname(name), tuple(int(i) for i in idx))

    def getarr(self, name, idx):
        n = self.vname(name); a = self.arrays.get(n)
        if a is None: a = self.arrays[n] = {"dim": (10,) * len(idx), "v": {}}
        for i, d in zip(idx, a["dim"]):
            if int(i) < 0 or int(i) > d: raise BasicError("bad subscript %s%s" % (n, idx))
        return a

    # ---- expressions ----
    TOK = re.compile(r'\s*(?:(?P<num>\d+\.?\d*|\.\d+)|(?P<str>"[^"]*")|(?P<id>[a-z][a-z0-9]*\$?)|(?P<op><>|<=|>=|[-+*/^()=<>,;#]))', re.I)

    def tokens(self, s):
        out, pos = [], 0
        s = s.strip()
        while pos < len(s):
            m = self.TOK.match(s, pos)
            if not m: raise BasicError("cannot tokenize %r at %r" % (s, s[pos:]))
            pos = m.end()
            if m.group("num") is not None: out.append(("num", float(m.group("num"))))
            elif m.group("str") is not None: out.append(("str", self.petscii(m.group("str")[1:-1])))
            elif m.group("id") is not None: out.append(("id", m.group("id").lower()))
            else: out.append(("op", m.group("op")))
        return out

    @staticmethod
    def petscii(s): return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in s)

    def eval(self, s):
        self.t = self.tokens(s); self.p = 0
        v = self.e_or()
        if self.p != len(self.t): raise BasicError("trailing tokens in %r: %r" % (s, self.t[self.p:]))
        return v

    def peek_tok(self): return self.t[self.p] if self.p < len(self.t) else (None, None)
    def take(self): t = self.t[self.p]; self.p += 1; return t

    def e_or(self):
        v = self.e_and()
        while self.peek_tok() == ("id", "or"):
            self.take(); w = self.e_and(); v = int(v) | int(w)
        return v

    def e_and(self):
        v = self.e_cmp()
        while self.peek_tok() == ("id", "and"):
            self.take(); w = self.e_cmp(); v = int(v) & int(w)
        return v

    def e_cmp(self):
        v = self.e_add()
        while self.peek_tok()[0] == "op" and self.peek_tok()[1] in ("=", "<>", "<", ">", "<=", ">="):
            op = self.take()[1]; w = self.e_add()
            if isinstance(v, str) != isinstance(w, str): raise BasicError("type mismatch in compare")
            r = {"=": v == w, "<>": v != w, "<": v < w, ">": v > w, "<=": v <= w, ">=": v >= w}[op]
            v = -1 if r else 0
        return v

    def e_add(self):
        v = self.e_mul()
        while self.peek_tok()[0] == "op" and self.peek_tok()[1] in ("+", "-"):
            op = self.take()[1]; w = self.e_mul()
            if op == "+":
                if isinstance(v, str) != isinstance(w, str): raise BasicError("type mismatch in +")
                v = v + w
            else: v = v - w
        return v

    def e_mul(self):
        v = self.e_un()
        while self.peek_tok()[0] == "op" and self.peek_tok()[1] in ("*", "/"):
            op = self.take()[1]; w = self.e_un(); v = v * w if op == "*" else v / w
        return v

    def e_un(self):
        if self.peek_tok() == ("op", "-"): self.take(); return -self.e_un()
        if self.peek_tok() == ("op", "+"): self.take(); return self.e_un()
        return self.e_atom()

    def args(self):
        out = []
        assert self.take() == ("op", "(")
        while True:
            out.append(self.e_or())
            t = self.take()
            if t == ("op", ")"): return out
            assert t == ("op", ","), t

    def e_atom(self):
        k, v = self.take()
        if k == "num": return int(v) if v == int(v) else v
        if k == "str": return v
        if k == "op" and v == "(":
            r = self.e_or(); assert self.take() == ("op", ")"); return r
        if k != "id": raise BasicError("unexpected %r" % (v,))
        if self.peek_tok() == ("op", "("):
            a = self.args()
            f = v
            if f == "chr$": return chr(int(a[0]))
            if f == "asc":
                if a[0] == "": raise BasicError("ILLEGAL QUANTITY (asc of empty string)")
                return ord(a[0][0])
            if f == "len": return len(a[0])
            if f == "left$": return a[0][:int(a[1])]
            if f == "right$": return a[0][-int(a[1]):] if int(a[1]) > 0 else ""
            if f == "mid$":
                st = int(a[1])
                if st < 1: raise BasicError("ILLEGAL QUANTITY (mid$ start)")
                return a[0][st - 1:] if len(a) == 2 else a[0][st - 1:st - 1 + int(a[2])]
            if f == "str$": return (" " if a[0] >= 0 else "-") + (str(int(abs(a[0]))) if a[0] == int(a[0]) else str(abs(a[0])))
            if f == "val":
                m = re.match(r"\s*([-+]?\d+\.?\d*)", a[0]); return (int(float(m.group(1))) if m and float(m.group(1)) == int(float(m.group(1))) else (float(m.group(1)) if m else 0))
            if f == "peek": return self.mem.get(int(a[0]), 0)
            if f == "int": return int(a[0] // 1)
            if f == "spc": return " " * int(a[0])
            arr = self.getarr(f, a)
            return arr["v"].get(tuple(int(i) for i in a), "" if f.endswith("$") else 0)
        return self.getvar(v)

    # ---- statements ----
    def assign_target(self, s):
        m = re.match(r"\s*([a-z][a-z0-9]*\$?)\s*(\((.*)\))?\s*$", s.strip(), re.I)
        if not m: raise BasicError("bad target %r" % s)
        return m.group(1).lower(), m.group(3)

    def store(self, target, value):
        name, idx = self.assign_target(target)
        if idx is None: self.setvar(name, value)
        else:
            ix = [self.eval(i) for i in split_top(idx, ",")]
            a = self.getarr(name, ix)
            if name.endswith("$") != isinstance(value, str): raise BasicError("type mismatch %s" % name)
            a["v"][tuple(int(i) for i in ix)] = value

    def out(self, text):
        for ch in text: self.screen.put(ord(ch))

    def split_print(self, rest):
        """-> list of values and a flag: ends with ; or ,"""
        items, cur, q, depth = [], "", False, 0
        seps = []
        for ch in rest:
            if ch == '"': q = not q
            if not q:
                if ch == "(": depth += 1
                if ch == ")": depth -= 1
                if ch in ";," and depth == 0: items.append(cur); seps.append(ch); cur = ""; continue
            cur += ch
        tail = cur.strip() != ""
        if tail: items.append(cur)
        return items, seps, tail

    def exec_stmt(self, s):
        """returns None to continue, or 'skipline' """
        s0 = s.strip()
        if not s0: return None
        low = s0.lower()
        kw = next((k for k in KEYWORDS if low.startswith(k)), None)
        if kw is None:
            tgt, _, expr = s0.partition("=")
            self.store(tgt, self.eval(expr)); return None
        rest = s0[len(kw):]
        if kw in ("rem", "data"): return None
        if kw == "goto": self.goto(int(rest)); return "jumped"
        if kw == "gosub":
            self.gosubs.append((self.li, self.si + 1, len(self.fors))); self.goto(int(rest)); return "jumped"
        if kw == "return":
            if not self.gosubs: raise BasicError("RETURN without GOSUB")
            li, si, nf = self.gosubs.pop(); del self.fors[nf:]; self.li, self.si = li, si; return "jumped"
        if kw == "end": self.ended = True; return "jumped"
        if kw == "if":
            i = find_top(rest, "then")
            if i < 0: raise BasicError("IF without THEN: " + s0)
            cond, then = rest[:i], rest[i + 4:].strip()
            if self.eval(cond) == 0: return "skipline"
            if re.match(r"^\d+$", then): self.goto(int(then)); return "jumped"
            return self.exec_stmt(then)
        if kw == "for":
            m = re.match(r"\s*([a-z][a-z0-9]*)\s*=\s*(.+?)\s+to\s+(.+)$", rest, re.I)
            if not m: raise BasicError("bad FOR " + s0)
            var = self.vname(m.group(1)); self.setvar(var, self.eval(m.group(2)))
            self.fors = [f for f in self.fors if f[0] != var]
            self.fors.append((var, self.eval(m.group(3)), self.li, self.si + 1)); return None
        if kw == "next":
            var = self.vname(rest.strip()) if rest.strip() else (self.fors[-1][0] if self.fors else None)
            if not self.fors: raise BasicError("NEXT without FOR")
            while self.fors and self.fors[-1][0] != var: self.fors.pop()
            if not self.fors: raise BasicError("NEXT without FOR " + str(var))
            v, lim, li, si = self.fors[-1]
            self.setvar(v, self.getvar(v) + 1)
            if self.getvar(v) <= lim: self.li, self.si = li, si; return "jumped"
            self.fors.pop(); return None
        if kw == "dim":
            for d in split_top(rest, ","):
                m = re.match(r"\s*([a-z][a-z0-9]*\$?)\((.*)\)\s*$", d, re.I)
                self.arrays[self.vname(m.group(1))] = {"dim": tuple(int(self.eval(x)) for x in split_top(m.group(2), ",")), "v": {}}
            return None
        if kw == "read":
            for t in split_top(rest, ","):
                v = self.data[self.dp]; self.dp += 1; self.store(t, v if not isinstance(v, str) or t.strip().endswith("$") or True else v)
            return None
        if kw == "poke":
            a, b = split_top(rest, ","); self.mem[int(self.eval(a))] = int(self.eval(b)) & 255; return None
        if kw == "sys":
            if int(self.eval(rest)) == 65520: self.screen.r, self.screen.c = self.mem.get(781, 0), self.mem.get(782, 0)
            else: raise BasicError("unexpected SYS " + rest)
            return None
        if kw == "get":
            if not self.keys: raise NeedKey()
            k = self.keys.pop(0); self.store(rest, chr(k) if k else ""); return None
        if kw == "get#":
            lf, _, tgt = rest.partition(","); ch = self.io.get(int(self.eval(lf)))
            self.store(tgt, ch); return None
        if kw == "input#":
            parts = split_top(rest, ","); lf = int(self.eval(parts[0])); line = self.io.getline(lf)
            vals = line.split(","); 
            for t, v in zip(parts[1:], vals):
                name, _ = self.assign_target(t)
                self.store(t, v if name.endswith("$") else (int(float(v.strip() or 0))))
            return None
        if kw == "open":
            parts = split_top(rest, ",")
            self.io.open(int(self.eval(parts[0])), int(self.eval(parts[1])), int(self.eval(parts[2])), self.eval(parts[3]) if len(parts) > 3 else "")
            return None
        if kw == "close":
            for lf in split_top(rest, ","): self.io.close(int(self.eval(lf)))
            return None
        if kw == "print#":
            parts = split_top(rest, ","); lf = int(self.eval(parts[0])); text = ",".join(parts[1:])
            items, seps, tail = self.split_print(text)
            self.io.put(lf, "".join(str(self.eval(i)) if not isinstance(self.eval(i), str) else self.eval(i) for i in items) + ("" if (seps and not tail) else "\r"))
            return None
        if kw == "print":
            items, seps, tail = self.split_print(rest)
            for i in items:
                if i.strip() == "": continue
                v = self.eval(i); self.out(v if isinstance(v, str) else ((" " if v >= 0 else "-") + str(abs(int(v))) + " "))
            if not (seps and not tail): self.out("\r")
            return None
        raise BasicError("unhandled " + s0)

    def goto(self, num):
        if num not in self.index: raise BasicError("UNDEF'D STATEMENT %d" % num)
        self.li, self.si = self.index[num], 0

    def run(self, max_steps=5_000_000):
        """run until the program ends or wants a key it does not have; returns 'end' or 'key'"""
        while not self.ended:
            if self.li >= len(self.lines): self.ended = True; break
            num, stmts = self.lines[self.li]
            if self.si >= len(stmts): self.li += 1; self.si = 0; continue
            self.steps += 1
            if self.steps > max_steps: raise BasicError("runaway program")
            try:
                r = self.exec_stmt(stmts[self.si])
            except NeedKey:
                return "key"
            except BasicError as e:
                raise BasicError("line %d: %s" % (num, e))
            if r == "jumped": continue
            if r == "skipline": self.li += 1; self.si = 0; continue
            self.si += 1
        return "end"

    def press(self, *keys):
        for k in keys:
            if isinstance(k, str): self.keys.extend(ord(self.petscii(c)) for c in k)     # unshifted keys give the upper case codes
            else: self.keys.append(k)
