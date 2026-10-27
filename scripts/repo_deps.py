#!/usr/bin/env python3
"""Import edges and the tests a change reaches — the `affected` half of the per-repo index.

Index time (per file, with the rest of the blob): `deps_of` keeps each code file's
raw import specifiers — Python `import`/`from`, TS/JS `from`/`import(…)`/`require`,
C/C++ `#include "…"`, shell `source`/`.`. Query time: `resolve_graph` turns them
into file → file edges against the indexed tree, and `affected` walks the importers
backwards from the changed files to the test files (a test ends its branch).

Rust and Move are tested per package (`cargo test -p`, `sui move test`), so they go
by package: a change inside a Cargo.toml / Move.toml package reaches that package,
every package depending on it through a `path =` / `local =` dependency, and the
test files inside them.

Not seen: a test that runs code as a subprocess or reads a file by a path string,
and imports through an alias the resolver does not know (tsconfig paths, PYTHONPATH).
"""
import json
import os
import posixpath
import re
from collections import deque

_PY_IMPORT = re.compile(r"^[ \t]*import[ \t]+([\w. \t,]+)", re.M)
_PY_FROM = re.compile(r"^[ \t]*from[ \t]+(\.*[\w.]*)[ \t]+import[ \t]+(\([^)]*\)|[^\n#]*)", re.M)
_JS = re.compile(r"(?:\bfrom\s*|\bimport\s*\(?\s*|\brequire\s*\(\s*)(['\"])([^'\"\n]+)\1")
_INC = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)
_SH = re.compile(r"^[ \t]*(?:source|\.)[ \t]+([^\n;#|&]+)", re.M)
_SH_PREFIX = re.compile(r"""["']|\$\([^)]*\)/?|\$\{?\w+\}?/?""")
_PY_EXT, _SH_EXT = {".py"}, {".sh", ".bash"}
_JS_EXT = (".ts", ".tsx", ".mts", ".cts", ".js", ".jsx", ".mjs", ".cjs")
_C_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
MANIFESTS = {"Cargo.toml": ("cargo", ".rs", r"\bpath\s*=\s*\"([^\"]+)\""),
             "Move.toml": ("move", ".move", r"\blocal\s*=\s*\"([^\"]+)\"")}


def is_test_path(path):
    """A test suite by the conventions ecosystems share (codegraph's isTestPath, narrowed):
    test_x.py, x_test.go, x.test.ts, x.spec.tsx, x_unittest.cc, FooTest.kt, and anything
    under tests/, test/, __tests__/, spec/, e2e/."""
    lower, name = path.lower(), posixpath.basename(path)
    ln = name.lower()
    if ln.startswith(("test_", "test.")) \
            or re.search(r"[._-](test|tests|unittest|unittests|spec|specs)\.[a-z0-9]+$", ln) \
            or re.search(r"(Test|Tests|TestCase|Spec|Specs)\.[A-Za-z0-9]+$", name):
        return True
    parts = lower.split("/")[:-1]
    return any(p in ("tests", "test", "__tests__", "spec", "specs", "e2e") for p in parts)


def deps_of(path, text):
    """-> [(spec, kind)] raw import specifiers of one code file."""
    ext = posixpath.splitext(path)[1].lower()
    out = []
    if ext in _PY_EXT:
        for m in _PY_IMPORT.finditer(text):
            for part in m.group(1).split(","):
                mod = part.split()[0] if part.split() else ""
                if mod:
                    out.append((mod, "py"))
        for m in _PY_FROM.finditer(text):
            mod = m.group(1)
            out.append((mod, "py"))
            for part in m.group(2).strip("()").replace("\n", ",").split(","):
                name = part.split()[0] if part.split() else ""
                if re.match(r"^\w+$", name):
                    out.append((mod + name if mod.endswith(".") else mod + "." + name, "py"))
    elif ext in _JS_EXT:
        out += [(m.group(2), "js") for m in _JS.finditer(text)]
    elif ext in _C_EXT:
        out += [(m.group(1), "inc") for m in _INC.finditer(text)]
    elif ext in _SH_EXT:
        for m in _SH.finditer(text):
            words = _SH_PREFIX.sub("", m.group(1).strip()).split()
            spec = words[0] if words else ""
            if spec and not spec.startswith("-"):
                out.append((spec, "sh"))
    return list(dict.fromkeys(out))


class Tree:
    """The indexed paths, with the lookups resolution needs."""

    def __init__(self, paths, manifests):
        self.paths = set(paths)
        self.by_suffix = {}
        for p in self.paths:
            self.by_suffix.setdefault(posixpath.basename(p), []).append(p)
        self.workspaces = {}  # npm package name -> [(dir, package.json)]
        for path, text in manifests.items():
            if posixpath.basename(path) == "package.json":
                try:
                    pj = json.loads(text)
                except ValueError:
                    continue
                if isinstance(pj, dict) and isinstance(pj.get("name"), str):
                    self.workspaces.setdefault(pj["name"], []).append(
                        (posixpath.dirname(path), pj))

    def workspace(self, name, src):
        """The package named `name` nearest the importer `src` (copies of a package can
        share a name): most leading dirs shared, then the shallowest."""
        def shared(d):
            a, b = d.split("/"), src.split("/")
            n = 0
            while n < min(len(a), len(b)) and a[n] == b[n]:
                n += 1
            return n
        return max(self.workspaces[name], key=lambda w: (shared(w[0]), -w[0].count("/")))

    def suffix(self, spec):
        """Every path ending in /spec (an include or a sourced script not found beside it)."""
        return [p for p in self.by_suffix.get(posixpath.basename(spec), [])
                if p == spec or p.endswith("/" + spec)]

    def js_file(self, base):
        if base in self.paths:
            return [base]
        stem, ext = posixpath.splitext(base)
        tries = [stem + e for e in _JS_EXT] if ext in _JS_EXT else []
        tries += [base + e for e in _JS_EXT] + [base + "/index" + e for e in _JS_EXT]
        return [t for t in tries if t in self.paths][:1]


def _norm(dirname, spec):
    p = posixpath.normpath(posixpath.join(dirname, spec))
    return None if p.startswith("..") or p == "." else p


def _export_target(value):
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        for k in ("source", "import", "default", "require", "types", "node"):
            t = _export_target(value.get(k))
            if t:
                return t
        for v in value.values():
            t = _export_target(v)
            if t:
                return t
    return None


def _resolve_js(tree, src, spec):
    if spec.startswith((".", "/")):
        p = _norm(posixpath.dirname(src), spec)
        return tree.js_file(p) if p else []
    parts = spec.split("/")
    name = "/".join(parts[:2]) if spec.startswith("@") else parts[0]
    if name not in tree.workspaces:
        return []
    root, pj = tree.workspace(name, src)
    sub = spec[len(name):].lstrip("/")
    exports = pj.get("exports")
    if isinstance(exports, (str, dict)):
        if isinstance(exports, dict) and any(k.startswith(".") for k in exports):
            target = _export_target(exports.get("./" + sub if sub else "."))
        else:
            target = _export_target(exports) if not sub else None
        if target:
            p = _norm(root, target)
            return tree.js_file(p) if p else []
    if sub:
        return tree.js_file(posixpath.join(root, sub)) or \
            tree.js_file(posixpath.join(root, "src", sub))
    main = pj.get("source") or pj.get("module") or pj.get("main")
    for base in ([_norm(root, main)] if isinstance(main, str) else []) + \
            [posixpath.join(root, "src/index"), posixpath.join(root, "index")]:
        hit = tree.js_file(base) if base else []
        if hit:
            return hit
    return []


def _resolve_py(tree, src, spec):
    dots = len(spec) - len(spec.lstrip("."))
    rel = spec[dots:].replace(".", "/")
    if dots:
        base = posixpath.dirname(src)
        for _ in range(dots - 1):
            base = posixpath.dirname(base)
        roots = [base]
    else:
        roots, d = [], posixpath.dirname(src)
        while True:
            roots.append(d)
            if not d:
                break
            d = posixpath.dirname(d)
    for root in roots:
        stem = posixpath.join(root, rel) if rel else root
        for cand in (stem + ".py", posixpath.join(stem, "__init__.py")):
            cand = cand.lstrip("/")
            if cand in tree.paths:
                return [cand]
    return []


def resolve(tree, src, spec, kind):
    d = posixpath.dirname(src)
    if kind == "py":
        return _resolve_py(tree, src, spec)
    if kind == "js":
        return _resolve_js(tree, src, spec)
    if kind in ("inc", "sh"):
        p = _norm(d, spec)
        return [p] if p in tree.paths else tree.suffix(spec.lstrip("./"))
    return []


def importers(tree, deps):
    """deps: {src path: [(spec, kind)]} -> {target path: {importer paths}}."""
    rev = {}
    for src, specs in deps.items():
        for spec, kind in specs:
            for t in resolve(tree, src, spec, kind):
                if t != src:
                    rev.setdefault(t, set()).add(src)
    return rev


def packages(tree, manifests):
    """-> {dir: {"kind", "name", "deps": [dir]}} for every Cargo.toml / Move.toml."""
    out = {}
    for path, text in manifests.items():
        base = posixpath.basename(path)
        if base not in MANIFESTS:
            continue
        kind, _, dep_re = MANIFESTS[base]
        d = posixpath.dirname(path)
        m = re.search(r"^\s*name\s*=\s*\"([^\"]+)\"", text, re.M)
        deps = [posixpath.normpath(posixpath.join(d, p)) for p in re.findall(dep_re, text)]
        out[d] = {"kind": kind, "name": m.group(1) if m else posixpath.basename(d),
                  "deps": ["" if p == "." else p for p in deps if not p.startswith("..")]}
    return out


def package_of(pkgs, path):
    """The nearest package holding `path` whose language it is (a .rs file → a Cargo
    package, a .move file → a Move package; a manifest is its own package)."""
    base, ext = posixpath.basename(path), posixpath.splitext(path)[1].lower()
    d = posixpath.dirname(path)
    while True:
        p = pkgs.get(d)
        if p:
            mf = "Cargo.toml" if p["kind"] == "cargo" else "Move.toml"
            if base == mf or ext == MANIFESTS[mf][1]:
                return d
        if not d:
            return None
        d = posixpath.dirname(d)


def affected(tree, deps, manifests, changed):
    """-> {"tests": [{path, origin, hops}], "packages": [{path, kind, name, origin, hops}],
    "unreached": [changed paths that reach no test and no package]}."""
    rev = importers(tree, deps)
    seen = {c: (c, 0) for c in changed}
    queue = deque(c for c in changed if not is_test_path(c))
    while queue:
        cur = queue.popleft()
        origin, hops = seen[cur]
        for imp in sorted(rev.get(cur, ())):
            if imp not in seen:
                seen[imp] = (origin, hops + 1)
                if not is_test_path(imp):
                    queue.append(imp)
    tests = {p: v for p, v in seen.items() if is_test_path(p)}

    pkgs = packages(tree, manifests)
    rdeps = {}
    for d, p in pkgs.items():
        for dep in p["deps"]:
            rdeps.setdefault(dep, set()).add(d)
    hit = {}
    for c in changed:
        d = package_of(pkgs, c)
        if d is not None and d not in hit:
            hit[d] = (c, 0)
    queue = deque(hit)
    while queue:
        cur = queue.popleft()
        origin, hops = hit[cur]
        for d in sorted(rdeps.get(cur, ())):
            if d not in hit:
                hit[d] = (origin, hops + 1)
                queue.append(d)
    if hit:
        for path in tree.paths:
            d = package_of(pkgs, path)
            if d in hit and is_test_path(path[len(d):]) and path not in tests:
                tests[path] = hit[d]

    reached = {o for o, _ in tests.values()} | {o for o, _ in hit.values()}
    return {
        "tests": [{"path": p, "origin": o, "hops": h} for p, (o, h) in sorted(tests.items())],
        "packages": [{"path": d, "kind": pkgs[d]["kind"], "name": pkgs[d]["name"],
                      "origin": o, "hops": h} for d, (o, h) in sorted(hit.items())],
        "unreached": sorted(c for c in changed if c not in reached),
    }
