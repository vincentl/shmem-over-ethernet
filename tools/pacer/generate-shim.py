#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""generate-shim.py - build the OpenSHMEM profiling-interface counting shim.

Usage: generate-shim.py PREPROCESSED_PSHMEM_H OUTPUT_C

The osoe library defines every routine as a strong `pshmem_NAME`, with
`shmem_NAME` a weak alias to it (spec section 10, the profiling interface). A
program that defines its own strong `shmem_NAME` therefore intercepts every
call through that entry point and can still reach the real implementation by
calling `pshmem_NAME`.

This script reads PREPROCESSED_PSHMEM_H - the output of running the C
preprocessor over shmem/include/pshmem.h, for example:

    cc -E -P -I shmem/include -I <build dir>/shmem/include shmem/include/pshmem.h

- and writes OUTPUT_C: a C file defining one `shmem_NAME` per declared
`pshmem_NAME`, each of which counts the call (one atomic counter per routine)
and forwards to `pshmem_NAME`, plus a `pacer_shim_report()` that sums those
counters across every PE and prints which routines were, and were not,
exercised.

pshmem.h's declarations are themselves produced by X-macro type lists (the
same lists shmem.h uses to declare the un-prefixed routines), so a routine
cannot be found by reading pshmem.h as text - only by reading what the
preprocessor expands those lists into. Hence the preprocessed input, never
pshmem.h itself.
"""

import re
import sys

# Annex F routines that shmem.h declares with no shmem_ prefix: pshmem.h's
# names for them are a second profiling spelling. A program calls them by these
# names, so the shim defines these names - a shmem_start_pes would count
# nothing, since nothing calls it.
UNPREFIXED = {"start_pes", "_my_pe", "_num_pes", "shmalloc", "shfree", "shrealloc",
              "shmemalign"}


def public_name(name):
    """The name a program calls for pshmem_NAME."""
    return name if name in UNPREFIXED else "shmem_" + name


PSHMEM_NAME_RE = re.compile(
    r"(?<![A-Za-z0-9_])pshmem_([A-Za-z_][A-Za-z0-9_]*)\s*\("
)

# The trailing identifier of a parameter declaration, e.g. "const T *cmp_value"
# -> "cmp_value", "T pWrk[]" -> "pWrk" (no array parameter exists in this
# header today, but the pattern costs nothing to handle).
PARAM_NAME_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?$")


def normalize_whitespace(text):
    """Collapse any run of whitespace (including newlines from a
    declaration that spans lines) to a single space, and strip the ends."""
    return " ".join(text.split())


def find_matching_paren(text, open_index):
    """text[open_index] must be '('. Return the index of the ')' that closes
    it, or -1 if the parentheses in text are not balanced from there on."""
    depth = 0
    for i in range(open_index, len(text)):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def split_top_level(text, sep=","):
    """Split text on sep, but not inside (...) or [...] nesting. None of
    today's declarations need the bracket handling, but a parameter list is
    exactly the place a future function-pointer or array parameter would
    introduce one, so the split stays safe if that ever happens."""
    parts = []
    depth = 0
    start = 0
    for i, c in enumerate(text):
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p.strip() for p in parts]


def parameter_name(param_text):
    """Return the parameter's name, '...' for a variadic marker, or None for
    a bare 'void' (no parameters) or anything unrecognizable."""
    param_text = param_text.strip()
    if param_text in ("", "void"):
        return None
    if param_text == "...":
        return "..."
    m = PARAM_NAME_RE.search(param_text)
    if not m:
        return None
    return m.group(1)


class Routine:
    __slots__ = ("name", "rettype", "params_text", "noreturn", "variadic", "param_names")


def parse_declarations(text, warn):
    """Parse every `pshmem_NAME(...)` function declaration out of the
    preprocessed text. Returns a dict of name -> Routine.

    The preprocessed text is a flat sequence of statements, each ending in
    ';' (the X-macro bodies emit one ';'-terminated declaration at a time,
    however many are packed onto one physical output line). So splitting on
    ';' first, then normalizing whitespace within each piece, turns "a
    declaration that spans lines, or many declarations sharing a line," into
    one clean string per declaration - without needing to understand the
    macros that produced them.
    """
    routines = {}
    for raw_stmt in text.split(";"):
        stmt = normalize_whitespace(raw_stmt)
        if not stmt or "pshmem_" not in stmt:
            continue
        m = PSHMEM_NAME_RE.search(stmt)
        if not m:
            # Contains the substring "pshmem_" but not as a function being
            # declared here (for instance, inside a comment would have been
            # stripped already by the preprocessor; this is just defensive).
            continue
        name = m.group(1)

        rettype = stmt[: m.start()].strip()
        noreturn = False
        if rettype == "_Noreturn" or rettype.startswith("_Noreturn "):
            # e.g. "_Noreturn void" -> keep "void" as the return type and
            # remember to re-apply _Noreturn to our own definition, so a
            # noreturn pshmem_ routine (shmem_global_exit) still reads as
            # noreturn at its shmem_ entry point and the compiler does not
            # warn that a noreturn function "returns".
            rettype = rettype[len("_Noreturn"):].strip()
            noreturn = True
        if not rettype:
            warn(f"pshmem_{name}: no return type found in {stmt!r}; skipped")
            continue
        if "{" in rettype or "}" in rettype:
            warn(f"pshmem_{name}: braces before the name, not a plain declaration; skipped")
            continue

        open_paren = m.end() - 1
        close_paren = find_matching_paren(stmt, open_paren)
        if close_paren == -1:
            warn(f"pshmem_{name}: unbalanced parentheses; skipped")
            continue
        params_text = normalize_whitespace(stmt[open_paren + 1 : close_paren])
        trailing = stmt[close_paren + 1 :].strip()
        if trailing:
            warn(f"pshmem_{name}: unexpected text {trailing!r} after the parameter list; skipped")
            continue
        if "{" in params_text or "}" in params_text:
            warn(f"pshmem_{name}: braces in the parameter list, not a plain declaration; skipped")
            continue

        variadic = False
        param_names = []
        if params_text != "" and params_text != "void":
            ok = True
            for p in split_top_level(params_text):
                if p == "...":
                    variadic = True
                    continue
                pname = parameter_name(p)
                if pname is None:
                    warn(f"pshmem_{name}: could not find a parameter name in {p!r}; skipped")
                    ok = False
                    break
                param_names.append(pname)
            if not ok:
                continue

        if name in routines:
            existing = routines[name]
            if (existing.rettype, existing.params_text) != (rettype, params_text):
                warn(f"pshmem_{name}: seen twice with different signatures; keeping the first")
            continue

        r = Routine()
        r.name = name
        r.rettype = rettype
        r.params_text = params_text
        r.noreturn = noreturn
        r.variadic = variadic
        r.param_names = param_names
        routines[name] = r

    return routines


def function_signature(r):
    """RET (shmem_NAME)(PARAMS) - the parentheses around the name suppress
    expansion for the handful of names (shmem_wait, shmem_sync, ...) that
    shmem.h also defines as a C11-generic-dispatch function-like macro
    (#define shmem_wait(...) ...). shmem.h itself relies on this exact trick
    for the same reason (see the comment above its own `shmem_wait`
    declaration), so it is applied here to every definition rather than
    singled out for the names known to collide today: a macro added later
    for some other name would otherwise break silently."""
    sig = f"({public_name(r.name)})({r.params_text})"
    if r.rettype.endswith("*"):
        sig = r.rettype + sig
    else:
        sig = r.rettype + " " + sig
    if r.noreturn:
        sig = "_Noreturn " + sig
    return sig


def emit(routines, out_path, source_note):
    ordered = sorted(routines.values(), key=lambda r: r.name)
    count = len(ordered)

    lines = []
    lines.append("/* SPDX-License-Identifier: LGPL-3.0-or-later */")
    lines.append("/*")
    lines.append(" * pacer-shim.c - GENERATED FILE. Do not edit by hand.")
    lines.append(" *")
    lines.append(" * The OpenSHMEM profiling interface's counting shim (spec section 10):")
    lines.append(" * every routine below is a strong `shmem_NAME`, overriding the library's")
    lines.append(" * weak alias of the same name, that counts the call and forwards it to")
    lines.append(" * `pshmem_NAME`. pacer_shim_report() sums the counts across every PE and")
    lines.append(" * writes which of the declared routines were called, and which were not.")
    lines.append(" *")
    lines.append(" * Generated by tools/pacer/generate-shim.py from")
    lines.append(f" * {source_note}.")
    lines.append(" * Regenerate rather than editing this file.")
    lines.append(" */")
    lines.append("#include <shmem.h>")
    lines.append("#include <pshmem.h>")
    lines.append("")
    lines.append("#include <stdatomic.h>")
    lines.append("#include <stdint.h>")
    lines.append("#include <stdio.h>")
    lines.append("#include <stdlib.h>")
    lines.append("#include <string.h>")
    lines.append("")
    lines.append(f"#define PACER_SHIM_COUNT {count}u")
    lines.append("")
    lines.append("static _Atomic unsigned long long pacer_shim_counts[PACER_SHIM_COUNT];")
    lines.append("")
    lines.append("static const char *const pacer_shim_names[PACER_SHIM_COUNT] = {")
    for r in ordered:
        lines.append(f'    "{public_name(r.name)}",')
    lines.append("};")
    lines.append("")

    for i, r in enumerate(ordered):
        args = ", ".join(n for n in r.param_names if n != "...")
        call = f"pshmem_{r.name}({args})"
        return_kw = "" if r.rettype == "void" else "return "
        lines.append(f"/* {i}: {public_name(r.name)} */")
        lines.append(function_signature(r))
        lines.append("{")
        lines.append(
            f"    atomic_fetch_add_explicit(&pacer_shim_counts[{i}], 1, memory_order_relaxed);"
        )
        lines.append(f"    {return_kw}{call};")
        lines.append("}")
        lines.append("")

    lines.append("/*")
    lines.append(" * Collective over SHMEM_TEAM_WORLD: every PE must call this before")
    lines.append(" * shmem_finalize. Calls only pshmem_ routines, so that producing the report")
    lines.append(" * does not itself add to the counts it reports.")
    lines.append(" */")
    lines.append("void pacer_shim_report(const char *path);   /* osoe-pacer.c calls it */")
    lines.append("")
    lines.append("void pacer_shim_report(const char *path)")
    lines.append("{")
    lines.append("    uint64_t *local_counts = pshmem_malloc(PACER_SHIM_COUNT * sizeof(*local_counts));")
    lines.append("    uint64_t *total_counts = pshmem_malloc(PACER_SHIM_COUNT * sizeof(*total_counts));")
    lines.append("    size_t i;")
    lines.append("")
    lines.append("    for (i = 0; i < PACER_SHIM_COUNT; i++) {")
    lines.append(
        "        local_counts[i] = atomic_load_explicit(&pacer_shim_counts[i], memory_order_relaxed);"
    )
    lines.append("    }")
    lines.append("")
    lines.append("    pshmem_uint64_sum_reduce(SHMEM_TEAM_WORLD, total_counts, local_counts,")
    lines.append("                             PACER_SHIM_COUNT);")
    lines.append("")
    lines.append("    if (pshmem_my_pe() == 0) {")
    lines.append("        FILE *out = path != NULL ? fopen(path, \"w\") : stderr;")
    lines.append("        size_t called = 0;")
    lines.append("")
    lines.append("        if (out == NULL) {")
    lines.append('            fprintf(stderr, "pacer_shim_report: cannot open %s, using stderr\\n", path);')
    lines.append("            out = stderr;")
    lines.append("        }")
    lines.append("        for (i = 0; i < PACER_SHIM_COUNT; i++) {")
    lines.append("            if (total_counts[i] > 0) {")
    lines.append("                called++;")
    lines.append("            }")
    lines.append("        }")
    lines.append('        fprintf(out, "shim called=%zu declared=%u\\n", called, PACER_SHIM_COUNT);')
    lines.append("        for (i = 0; i < PACER_SHIM_COUNT; i++) {")
    lines.append("            if (total_counts[i] > 0) {")
    lines.append(
        '                fprintf(out, "%s %llu\\n", pacer_shim_names[i],'
    )
    lines.append("                        (unsigned long long)total_counts[i]);")
    lines.append("            }")
    lines.append("        }")
    lines.append('        fprintf(out, "not-called:\\n");')
    lines.append("        for (i = 0; i < PACER_SHIM_COUNT; i++) {")
    lines.append("            if (total_counts[i] == 0) {")
    lines.append('                fprintf(out, "%s\\n", pacer_shim_names[i]);')
    lines.append("            }")
    lines.append("        }")
    lines.append("        if (out != stderr) {")
    lines.append("            fclose(out);")
    lines.append("        }")
    lines.append("    }")
    lines.append("")
    lines.append("    pshmem_free(local_counts);")
    lines.append("    pshmem_free(total_counts);")
    lines.append("}")
    lines.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))

    return count


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} PREPROCESSED_PSHMEM_H OUTPUT_C", file=sys.stderr)
        return 2

    in_path, out_path = argv[1], argv[2]

    with open(in_path, "r") as f:
        text = f.read()

    warnings = []

    def warn(msg):
        warnings.append(msg)

    routines = parse_declarations(text, warn)

    for msg in warnings:
        print(f"generate-shim.py: {msg}", file=sys.stderr)

    if not routines:
        print(
            f"generate-shim.py: found no pshmem_ declarations in {in_path}; "
            "was it really preprocessed?",
            file=sys.stderr,
        )
        return 1

    count = emit(routines, out_path, f"the preprocessed form of shmem/include/pshmem.h ({in_path})")

    variadic_names = sorted(r.name for r in routines.values() if r.variadic)
    print(
        f"generate-shim.py: wrote {out_path}: {count} routines wrapped, "
        f"{len(warnings)} declaration(s) skipped",
        file=sys.stderr,
    )
    if variadic_names:
        print(
            "generate-shim.py: variadic routine(s) counted but only their fixed "
            "arguments are forwarded (a '...' tail cannot be forwarded generically "
            f"in C): {', '.join('pshmem_' + n for n in variadic_names)}",
            file=sys.stderr,
        )

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
