#!/usr/bin/env python3
"""Static checks for manifest boundaries, ReXGlue errors, and fatal stubs.

Usage examples (all analysis is read-only):

  tools/check-manifest-integrity.py --selftest
  tools/check-manifest-integrity.py --manifest config/functions.toml \
      --image /path/to/tu-image.bin --generated out/auto-inputs/generated-snapshot \
      --codegen-log out/auto-inputs/codegen-c214.log

The image is a flat big-endian guest image based at 0x82000000.  Function
extents come from the image's .pdata records and generated instruction-comment
counts mapped through wwe2k14_init.cpp.  Manifest starts are checked against
direct branches, pointers in non-executable sections, and lis/addi|ori address
construction.  Exact switch-dispatch fragments without their own PDATA or
generated entry extent are high-confidence non-entry findings; other
no-reference starts are reported as suspicious, not silently removed or
treated as proof of a defect.  The checker also identifies dispatch starts
whose preceding conditional guard falls through into the dispatch, and entry
starts that land inside a recognizable instruction sequence.

Codegen [error] lines are grouped by their reported site and make normal
invocations exit non-zero.  --selftest runs the supplied pre-c214/c214 controls
and verifies the expected known-bad/fixed deltas without claiming that the
pre-c214 run is clean.
"""
import argparse
import collections
import glob
import os
import re
import struct
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000
DEFAULT_IMAGE = os.environ.get(
    "WWE2K14_IMAGE", "out/tu-image.bin")
DEFAULT_GENERATED = os.path.join(ROOT, "generated")
AUTO1 = os.path.join(ROOT, "out", "auto1")
PRE_MANIFEST = os.path.join(ROOT, "out", "auto-inputs", "functions.toml.pre-c214")
FIXED_MANIFEST = os.path.join(ROOT, "out", "auto-inputs", "functions.toml.c214")
PRE_LOG = os.path.join(ROOT, "out", "auto-inputs", "codegen-pre-c214.log")
FIXED_LOG = os.path.join(ROOT, "out", "auto-inputs", "codegen-c214.log")
# WWE '13's known pre-c214/c214 control addresses do not apply to 2K14. They
# are left empty so the pattern self-test runs on the synthetic recognizers
# only; the real 2K14 findings are the `WARN:no-start-reference` rows in
# out/audit/manifest-integrity.tsv.
EXPECTED_SPLITS = set()
EXPECTED_FALLTHROUGH_DISPATCHES = set()
EXPECTED_MIDSEQUENCE_ENTRIES = set()

MAP_RE = re.compile(rb"\{\s*0x([0-9A-Fa-f]{8}),\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}")
IMPL = "PPC_FUNC_IMPL(__imp__"
JUMP_ERROR_RE = re.compile(
    r"Jump target\s+(0x[0-9a-fA-F]+)\s+unresolved at bctr\s+(0x[0-9a-fA-F]+)")
COND_ERROR_RE = re.compile(
    r"Unresolved conditional branch to\s+(0x[0-9a-fA-F]+)\s+from\s+(0x[0-9a-fA-F]+)")
GENERIC_SITE_RE = re.compile(r"\b(?:from|at|site)\s+(0x[0-9a-fA-F]+)\b", re.IGNORECASE)
FATAL_RE = re.compile(r'REX_FATAL\s*\(\s*"([^"\n]*(?:Jump target|Unresolved stubs)[^"\n]*)"')
LIS_WINDOW = 12


def manifest_entries(path):
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    entries = []
    for key, value in doc.get("functions", {}).items():
        try:
            address = int(key, 0) if isinstance(key, str) else int(key)
        except (TypeError, ValueError):
            continue
        if not isinstance(value, dict):
            continue
        size = value.get("size", 0)
        if isinstance(size, str):
            size = int(size, 0)
        entries.append({
            "address": address,
            "size": int(size),
            "name": str(value.get("name", "")),
        })
    return sorted(entries, key=lambda e: e["address"])


def image_sections(image, image_path=None):
    """Return (name, start, end, executable) sections in loaded-image addresses.

    The 2K14 TU image is a raw section concatenation with no PE header, so when
    the PE parse yields nothing the `<image>.sections.txt` sidecar is used
    (NAME VA SIZE EXEC with absolute guest VAs).
    """
    sections = []
    try:
        pe = struct.unpack_from("<I", image, 0x3C)[0]
        count = struct.unpack_from("<H", image, pe + 6)[0]
        optional_size = struct.unpack_from("<H", image, pe + 20)[0]
        if not 0 < count < 64:
            raise ValueError
        table = pe + 24 + optional_size
        for index in range(count):
            offset = table + 40 * index
            name = image[offset:offset + 8].rstrip(b"\0").decode("latin1")
            size, rva = struct.unpack_from("<II", image, offset + 8)
            characteristics = struct.unpack_from("<I", image, offset + 36)[0]
            start = BASE + rva
            end = min(start + size, BASE + len(image))
            if start < end:
                sections.append((name, start, end, bool(characteristics & 0x20000000)))
    except (struct.error, IndexError, ValueError):
        sections = []
    if sections or not image_path:
        return sections
    sidecar = image_path + ".sections.txt"
    with open(sidecar) as fh:
        for line in fh:
            fields = line.split()
            if len(fields) != 4:
                continue
            name, va, size, executable = fields
            start = int(va, 0)
            end = min(start + int(size, 0), BASE + len(image))
            if start < end:
                sections.append((name, start, end, bool(int(executable, 0))))
    return sections


def pdata_extents(image, sections):
    """Decode Xbox PPC .pdata function-size records from the flat TU image."""
    pdata = next((section for section in sections if section[0] == ".pdata"), None)
    if pdata is None:
        return []
    _, start, end, _ = pdata
    extents = []
    for offset in range(start - BASE, end - BASE - 7, 8):
        address, flags = struct.unpack_from(">II", image, offset)
        if address == 0:
            break
        size = ((flags >> 8) & 0x3FFFFF) * 4
        if BASE <= address < BASE + len(image) and size and address + size <= BASE + len(image):
            extents.append((address, address + size, f"pdata_{address:08X}", ".pdata"))
    return extents


def generated_extents(generated):
    """Return mapped emitted body extents (start, end, symbol, source)."""
    init_path = os.path.join(generated, "wwe2k14_init.cpp")
    with open(init_path, "rb") as f:
        sym2addr = {sym.decode(): int(addr, 16) for addr, sym in MAP_RE.findall(f.read())}
    extents = []
    for path in sorted(glob.glob(os.path.join(generated, "wwe2k14_recomp.*.cpp"))):
        cur, count = None, 0

        def finish():
            if cur in sym2addr and count:
                start = sym2addr[cur]
                extents.append((start, start + count * 4, cur, os.path.basename(path)))

        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith(IMPL):
                    finish()
                    close = line.find(")", len(IMPL))
                    cur = line[len(IMPL):close] if close >= 0 else None
                    count = 0
                elif line.startswith("\t// ") and len(line) > 4 and line[4].islower():
                    count += 1
        finish()
    return sorted(extents)


def containing_body(extents, start, end):
    matches = [body for body in extents if body[0] < start and end < body[1]]
    return min(matches, key=lambda body: body[1] - body[0]) if matches else None


def image_word(image, address):
    offset = address - BASE
    if offset < 0 or offset + 4 > len(image) or offset & 3:
        return None
    return int.from_bytes(image[offset:offset + 4], "big")


def switch_dispatch(image, address):
    """Recognize the six-word switch-dispatch fragment used by the TU fixes."""
    words = [image_word(image, address + 4 * i) for i in range(6)]
    if any(w is None for w in words):
        return False, ""
    w0, w1, w2, w3, w4, w5 = words
    match = (
        (w0 & 0xFFFF0000) == 0x3D800000 and  # lis r12,hi
        (w1 & 0xFC1FFFFF) == 0x5400103A and  # rlwinm r0,rX,2,0,29
        (w2 & 0xFFFF0000) == 0x398C0000 and  # addi r12,r12,lo
        w3 == 0x7C0C002E and                 # lwzx r0,r12,r0
        w4 == 0x7C0903A6 and                 # mtctr r0
        w5 == 0x4E800420                    # bctr
    )
    return match, " ".join(f"{w:08X}" for w in words)


def dispatch_kind(image, address):
    """Recognize switch-table and computed-register dispatch entry shapes."""
    words = [image_word(image, address + 4 * i) for i in range(6)]
    if any(w is None for w in words):
        return "", ""
    is_switch, text = switch_dispatch(image, address)
    if is_switch:
        return "switch", text
    w0, w1, w2, w3, w4, w5 = words
    is_mtctr = (w4 >> 26) == 31 and ((w4 >> 1) & 0x3FF) == 467
    is_value_dispatch = (
        (w0 >> 26) == 21 and  # rlwinm/rotlwi index preparation
        (w1 >> 26) == 14 and  # addi
        (w2 >> 26) == 32 and  # load dispatch object
        (w3 >> 26) == 32 and  # load handler
        is_mtctr and w5 == 0x4E800420
    )
    return ("computed", text) if is_value_dispatch else ("", text)


def conditional_fallthrough_dispatch(image, address):
    """Describe a dispatch entered by fall-through after its conditional guard.

    A forward conditional branch that lands beyond a dispatcher is not a
    reference to the dispatcher: its not-taken edge enters the dispatcher.
    Conditional bclr forms (for example beqlr/bgtlr) are also guards because
    their not-taken edge falls through to the dispatch.
    """
    kind, _text = dispatch_kind(image, address)
    if not kind:
        return ""
    site = address - 4
    word = image_word(image, site)
    if word is None:
        return ""
    opcode = word >> 26
    bo = (word >> 21) & 31
    if opcode == 16 and bo != 20:  # conditional bc
        displacement = word & 0xFFFC
        if displacement & 0x8000:
            displacement -= 0x10000
        target = displacement if word & 2 else (site + displacement) & 0xFFFFFFFF
        dispatch_end = address + 24  # six instruction words
        if target > dispatch_end:
            return (f"conditional bc 0x{site:08X} targets 0x{target:08X}, past the "
                    f"six-word {kind} dispatch ending 0x{dispatch_end:08X}; fall-through enters it")
    elif (opcode == 19 and ((word >> 1) & 0x3FF) == 16 and
          not word & 1 and bo != 20):  # conditional bclr, not unconditional blr
        return (f"conditional bclr 0x{site:08X} (BO={bo}, BI={(word >> 16) & 31}) "
                f"guards the {kind} dispatch at 0x{address:08X}; its fall-through enters it")
    return ""


def entry_mid_sequence(image, address):
    """Identify an entry that starts after the beginning of a known sequence."""
    if address >= BASE + 8 and switch_dispatch(image, address - 8)[0]:
        return f"starts at word 3 of switch dispatch beginning 0x{address - 8:08X}"

    word = image_word(image, address)
    next_word = image_word(image, address + 4)
    after_next = image_word(image, address + 8)
    if word is None or next_word is None or after_next is None:
        return ""
    is_mtctr = (word >> 26) == 31 and ((word >> 1) & 0x3FF) == 467
    if is_mtctr and next_word == 0x4E800420:
        load = image_word(image, address - 12)
        compare = image_word(image, address - 8)
        guard = image_word(image, address - 4)
        is_lwzx = load is not None and (load >> 26) == 31 and ((load >> 1) & 0x3FF) == 23
        is_compare_imm = compare is not None and compare >> 26 == 10
        is_conditional_guard = (
            guard is not None and
            ((guard >> 26 == 16 and ((guard >> 21) & 31) != 20) or
             (guard >> 26 == 19 and ((guard >> 1) & 0x3FF) == 16 and
              not guard & 1 and ((guard >> 21) & 31) != 20))
        )
        if is_lwzx and is_compare_imm and is_conditional_guard:
            return f"starts at mtctr;bctr after lwzx/cmplwi/conditional guard at 0x{address - 12:08X}"

    is_addi_from_r3 = word >> 26 == 14 and ((word >> 16) & 31) == 3
    is_mtctr_next = (next_word >> 26) == 31 and ((next_word >> 1) & 0x3FF) == 467
    if is_addi_from_r3 and is_mtctr_next and after_next == 0x4E800420:
        load = image_word(image, address - 12)
        compare = image_word(image, address - 8)
        guard = image_word(image, address - 4)
        is_load = load is not None and (
            (load >> 26) == 32 or
            ((load >> 26) == 31 and ((load >> 1) & 0x3FF) == 23)
        )
        is_compare_imm = compare is not None and compare >> 26 == 10
        is_conditional_bclr = (
            guard is not None and guard >> 26 == 19 and
            ((guard >> 1) & 0x3FF) == 16 and not guard & 1 and
            ((guard >> 21) & 31) != 20
        )
        if (is_load and is_compare_imm and is_conditional_bclr and
                image_word(image, address + 12) == 0x4E800020):
            return f"starts at addi/mtctr/bctr after load/cmplwi/conditional bclr at 0x{address - 12:08X}"
    return ""


def image_references(image, addresses, sections):
    """Collect executable b/bl and lis-built refs plus non-executable pointers."""
    wanted = set(addresses)
    refs = collections.defaultdict(list)
    for _name, start, end, executable in sections:
        first = start - BASE
        limit = end - BASE
        if not executable:
            for offset in range(first, limit - 3, 4):
                value = struct.unpack_from(">I", image, offset)[0]
                if value in wanted:
                    refs[value].append(("data-pointer", BASE + offset))
            continue
        for offset in range(first, limit - 3, 4):
            word = struct.unpack_from(">I", image, offset)[0]
            site = BASE + offset
            if word >> 26 == 18:
                disp = word & 0x03FFFFFC
                if disp & 0x02000000:
                    disp -= 0x04000000
                target = disp if word & 2 else (site + disp) & 0xFFFFFFFF
                if target in wanted:
                    refs[target].append(("b/bl", site))
            if word >> 26 != 15 or ((word >> 16) & 31):  # lis/addis rD,0,hi
                continue
            rd = (word >> 21) & 31
            hi = (word & 0xFFFF) << 16
            for j in range(offset + 4, min(offset + 4 * (LIS_WINDOW + 1), limit - 3), 4):
                insn = struct.unpack_from(">I", image, j)[0]
                op = insn >> 26
                if op == 14 and ((insn >> 16) & 31) == rd:  # addi rX,rD,simm
                    lo = insn & 0xFFFF
                    value = (hi + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                elif op == 24 and ((insn >> 21) & 31) == rd:  # ori rX,rD,uimm
                    value = hi | (insn & 0xFFFF)
                else:
                    if ((op == 15 and ((insn >> 21) & 31) == rd) or
                            insn in (0x4E800020, 0x4E800420) or
                            (op == 18 and not insn & 1)):
                        break
                    continue
                if value in wanted:
                    refs[value].append(("lis+addi/ori", site))
    for target in refs:
        refs[target] = sorted(set(refs[target]))
    return refs


def classify_manifest(path, image_path, generated):
    entries = manifest_entries(path)
    with open(image_path, "rb") as f:
        image = f.read()
    sections = image_sections(image, image_path)
    generated_bodies = generated_extents(generated)
    extents = pdata_extents(image, sections) + generated_bodies
    refs = image_references(image, (entry["address"] for entry in entries), sections)
    rows = []
    for entry in entries:
        address, size = entry["address"], entry["size"]
        body = containing_body(extents, address, address + size)
        own_body = next((candidate for candidate in extents if candidate[0] == address), None)
        is_switch, words = switch_dispatch(image, address)
        mid_sequence = entry_mid_sequence(image, address)
        fallthrough = conditional_fallthrough_dispatch(image, address)
        evidence = refs.get(address, [])
        if is_switch and own_body is None:
            issue = "ERROR:split-switch-dispatch"
        elif body and not evidence and own_body is None:
            issue = "ERROR:unreferenced-interior-fragment"
        elif mid_sequence:
            issue = "WARN:entry-mid-sequence"
        elif fallthrough:
            issue = "WARN:conditional-fallthrough-dispatch"
        elif not evidence:
            issue = "WARN:no-start-reference"
        else:
            issue = "OK"
        if body:
            body_ev = f"inside {body[3]} {body[0]:08X}-{body[1]:08X} ({body[2]})"
        elif own_body:
            body_ev = f"own {own_body[3]} {own_body[0]:08X}-{own_body[1]:08X} ({own_body[2]})"
        else:
            body_ev = "no enclosing PDATA/generated extent"
        ref_ev = ",".join(f"{kind}@0x{site:08X}" for kind, site in evidence[:6]) or "none"
        rows.append({
            "address": address,
            "name": entry["name"],
            "size": size,
            "issue": issue,
            "body": body_ev,
            "references": ref_ev,
            "ref_count": len(evidence),
            "words": words or "out-of-image",
            "rule_detail": mid_sequence or fallthrough,
        })
    return rows


def parse_codegen_log(path):
    groups = collections.defaultdict(lambda: {"count": 0, "targets": collections.Counter(), "lines": []})
    errors = []
    for lineno, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
        if "[error]" not in line:
            continue
        errors.append((lineno, line.rstrip()))
        match = JUMP_ERROR_RE.search(line)
        if match:
            target, site = (int(x, 16) for x in match.groups())
            key = ("jump@bctr", site)
        else:
            match = COND_ERROR_RE.search(line)
            if match:
                target, site = (int(x, 16) for x in match.groups())
                key = ("conditional-branch", site)
            else:
                target = None
                site_match = GENERIC_SITE_RE.search(line)
                site = int(site_match.group(1), 16) if site_match else 0
                key = ("unclassified", site)
        group = groups[key]
        group["count"] += 1
        group["lines"].append(lineno)
        if target is not None:
            group["targets"][target] += 1
    return errors, groups


def generated_fatals(generated):
    rows = []
    for path in sorted(glob.glob(os.path.join(generated, "*.cpp"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                match = FATAL_RE.search(line)
                if match:
                    rows.append((os.path.basename(path), lineno, match.group(1)))
    return rows


def write_manifest_report(path, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("address\tname\tsize\tclassification\tenclosing_extent\tstart_references\treference_count\tfirst_six_words\trule_detail\n")
        for row in rows:
            f.write(f"0x{row['address']:08X}\t{row['name']}\t0x{row['size']:X}\t{row['issue']}\t"
                    f"{row['body']}\t{row['references']}\t{row['ref_count']}\t{row['words']}\t{row['rule_detail']}\n")


def write_codegen_report(path, log, errors, groups, fatals):
    jump_errors = sum(group["count"] for (kind, _), group in groups.items() if kind == "jump@bctr")
    jump_sites = sum(1 for kind, _ in groups if kind == "jump@bctr")
    cond_errors = sum(group["count"] for (kind, _), group in groups.items() if kind == "conditional-branch")
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"# Codegen integrity: `{os.path.basename(log)}`\n\n")
        f.write(f"- `[error]` lines: **{len(errors)}**\n- unresolved bctr jump errors: **{jump_errors}** across **{jump_sites}** bctr sites\n")
        f.write(f"- unresolved conditional-branch errors: **{cond_errors}**\n- generated fatal stubs found: **{len(fatals)}**\n\n")
        f.write("## Errors grouped by site\n\n| Type | Site | Count | Targets | Log lines |\n|---|---:|---:|---|---|\n")
        for (kind, site), group in sorted(groups.items(), key=lambda item: (item[0][0], item[0][1])):
            targets = ", ".join(f"0x{target:08X} ({count})" for target, count in sorted(group["targets"].items())) or "-"
            site_text = f"0x{site:08X}" if site else "unclassified"
            f.write(f"| {kind} | {site_text} | {group['count']} | {targets} | {','.join(map(str, group['lines']))} |\n")
        f.write("\n## Generated fatal stubs\n\n")
        if fatals:
            for source, lineno, message in fatals:
                f.write(f"- `{source}:{lineno}`: `{message}`\n")
        else:
            f.write("None found.\n")


def manifest_summary(rows):
    split_switches = {row["address"] for row in rows if row["issue"] == "ERROR:split-switch-dispatch"}
    errors = [row for row in rows if row["issue"].startswith("ERROR:")]
    warnings = [row for row in rows if row["issue"].startswith("WARN:")]
    return split_switches, errors, warnings


def synthetic_pattern_checks():
    """Exercise the new recognizers without relying on the TU image."""
    image = bytearray(0x1000)

    def put(address, word):
        struct.pack_into(">I", image, address - BASE, word)

    switch_words = (0x3D808228, 0x5560103A, 0x398C7BD8,
                    0x7C0C002E, 0x7C0903A6, 0x4E800420)
    switch_at = BASE + 0x100
    for index, word in enumerate(switch_words):
        put(switch_at + 4 * index, word)
    put(switch_at - 4, (16 << 26) | (12 << 21) | (25 << 16) | 0x200)
    forward_reason = conditional_fallthrough_dispatch(image, switch_at)

    put(switch_at - 4, (16 << 26) | (12 << 21) | (25 << 16) | 0x8)
    in_range_reason = conditional_fallthrough_dispatch(image, switch_at)

    computed_at = BASE + 0x200
    computed_words = (0x554B003E, 0x386B01B0, 0x816B01B0,
                      0x814B0008, 0x7D4903A6, 0x4E800420)
    for index, word in enumerate(computed_words):
        put(computed_at + 4 * index, word)
    put(computed_at - 4, 0x4D9A0020)
    bclr_reason = conditional_fallthrough_dispatch(image, computed_at)

    split_at = BASE + 0x300
    for index, word in enumerate(switch_words):
        put(split_at - 8 + 4 * index, word)
    switch_mid_reason = entry_mid_sequence(image, split_at)

    mtctr_at = BASE + 0x400
    put(mtctr_at - 12, 0x7C0C002E)  # lwzx
    put(mtctr_at - 8, 0x2B0B0000)  # cmplwi
    put(mtctr_at - 4, 0x4D9A0020)  # conditional bclr
    put(mtctr_at, 0x7D6903A6)  # mtctr
    put(mtctr_at + 4, 0x4E800420)  # bctr
    mtctr_mid_reason = entry_mid_sequence(image, mtctr_at)

    addi_at = BASE + 0x500
    put(addi_at - 12, 0x816719FC)  # lwz
    put(addi_at - 8, 0x2B0B0000)  # cmplwi
    put(addi_at - 4, 0x4D9A0020)  # conditional bclr
    for index, word in enumerate((0x38830030, 0x7D6903A6, 0x4E800420, 0x4E800020)):
        put(addi_at + 4 * index, word)
    addi_mid_reason = entry_mid_sequence(image, addi_at)

    return [
        (bool(forward_reason), "synthetic forward conditional branch falls through into switch dispatch"),
        (not in_range_reason, "synthetic conditional branch landing within dispatch is not classified as a skip"),
        (bool(bclr_reason), "synthetic conditional bclr guard falls through into computed dispatch"),
        (bool(switch_mid_reason), "synthetic third-word switch entry is classified as mid-sequence"),
        (bool(mtctr_mid_reason), "synthetic mtctr;bctr suffix is classified as mid-sequence"),
        (bool(addi_mid_reason), "synthetic guarded addi;mtctr;bctr tail is classified as mid-sequence"),
    ]


def manifest_pattern_checks(image_path, generated, manifest):
    current_rows = classify_manifest(manifest, image_path, generated)
    current_by_address = {row["address"]: row for row in current_rows}
    known_dispatches_ok = all(
        current_by_address.get(address, {}).get("issue") == "WARN:conditional-fallthrough-dispatch" and
        current_by_address.get(address, {}).get("rule_detail")
        for address in EXPECTED_FALLTHROUGH_DISPATCHES
    )
    known_midsequences_ok = all(
        current_by_address.get(address, {}).get("issue") == "WARN:entry-mid-sequence" and
        current_by_address.get(address, {}).get("rule_detail")
        for address in EXPECTED_MIDSEQUENCE_ENTRIES
    )
    return [
        (known_dispatches_ok,
         f"current manifest recognizes all {len(EXPECTED_FALLTHROUGH_DISPATCHES)} conditional-fallthrough dispatches"),
        (known_midsequences_ok,
         f"current manifest recognizes all {len(EXPECTED_MIDSEQUENCE_ENTRIES)} entry-mid-sequence tails"),
    ]


def selftest_patterns(manifest, image_path, generated):
    checks = synthetic_pattern_checks() + manifest_pattern_checks(image_path, generated, manifest)
    failures = []
    for passed, message in checks:
        print(f"[selftest-patterns] {'PASS' if passed else 'FAIL'} {message}")
        if not passed:
            failures.append(message)
    print(f"[selftest-patterns] checks={len(checks)} failures={len(failures)}")
    return not failures


def selftest(image, generated, report_dir):
    os.makedirs(report_dir, exist_ok=True)
    fatals = generated_fatals(generated)
    failures = []
    results = []
    for label, manifest, log in (("pre-c214", PRE_MANIFEST, PRE_LOG), ("c214", FIXED_MANIFEST, FIXED_LOG)):
        rows = classify_manifest(manifest, image, generated)
        mreport = os.path.join(report_dir, f"manifest-{label}.tsv")
        write_manifest_report(mreport, rows)
        split_switches, errors, warnings = manifest_summary(rows)
        log_errors, groups = parse_codegen_log(log)
        creport = os.path.join(report_dir, f"codegen-{label}.md")
        write_codegen_report(creport, log, log_errors, groups, fatals)
        jump_errors = sum(group["count"] for (kind, _), group in groups.items() if kind == "jump@bctr")
        jump_sites = sum(1 for kind, _ in groups if kind == "jump@bctr")
        cond_errors = sum(group["count"] for (kind, _), group in groups.items() if kind == "conditional-branch")
        results.append((label, rows, split_switches, errors, warnings, log_errors, jump_errors, jump_sites, cond_errors))

    pre, fixed = results
    pattern_checks = synthetic_pattern_checks() + manifest_pattern_checks(
        image, generated, os.path.join(ROOT, "config", "functions.toml"))
    for passed, message in pattern_checks:
        print(f"[selftest] {'PASS' if passed else 'FAIL'} {message}")
        if not passed:
            failures.append(message)

    expected_checks = [
        ({row["address"] for row in pre[3]} == EXPECTED_SPLITS and pre[2] == EXPECTED_SPLITS,
         f"pre-c214 manifest errors: expected exactly {len(EXPECTED_SPLITS)} switch-fragment addresses, got {len(pre[3])} errors/{len(pre[2])} switch fragments"),
        (len(pre[5]) == 137 and pre[6] == 125 and pre[7] == 7 and pre[8] == 12,
         f"pre-c214 log: expected 125 jump errors/7 bctr sites/12 conditional errors/137 total, got {pre[6]}/{pre[7]}/{pre[8]}/{len(pre[5])}"),
        (not fixed[3] and not fixed[5],
         f"c214 fixed controls: expected no manifest errors or [error] lines, got {len(fixed[3])} manifest errors and {len(fixed[5])} log errors"),
    ]
    for passed, message in expected_checks:
        print(f"[selftest] {'PASS' if passed else 'FAIL'} {message}")
        if not passed:
            failures.append(message)
    for label, rows, splits, errors, warnings, log_errors, jumps, sites, conds in results:
        print(f"[selftest] {label}: entries={len(rows)} split_switches={len(splits)} manifest_errors={len(errors)} "
              f"other_warnings={len(warnings)} log_errors={len(log_errors)} jump_errors={jumps} "
              f"bctr_sites={sites} conditional_errors={conds}")
    print(f"[selftest] generated REX_FATAL Jump target/Unresolved stubs: {len(fatals)}")
    print(f"[reports] {report_dir}")

    with open(os.path.join(report_dir, "manifest-integrity.md"), "w", encoding="utf-8") as f:
        f.write("# Manifest/codegen integrity self-test\n\n")
        f.write("| Manifest | Manifest entries | Split switch fragments | Manifest errors | Other warnings | Codegen errors | Jump errors / sites | Conditional errors |\n")
        f.write("|---|---:|---:|---:|---:|---:|---:|---:|\n")
        for label, rows, splits, errors, warnings, log_errors, jumps, sites, conds in results:
            f.write(f"| {label} | {len(rows)} | {len(splits)} | {len(errors)} | {len(warnings)} | {len(log_errors)} | {jumps} / {sites} | {conds} |\n")
        f.write(f"\nGenerated snapshot fatal matches: **{len(fatals)}**. See codegen reports for details.\n")
        f.write("\nThe high-confidence split addresses must be exactly: " + ", ".join(f"`0x{x:08X}`" for x in sorted(EXPECTED_SPLITS)) + ".\n")
    return not failures


def analyze(manifest, image, generated, log, report_dir):
    os.makedirs(report_dir, exist_ok=True)
    rows = classify_manifest(manifest, image, generated)
    manifest_path = os.path.join(report_dir, "manifest-integrity.tsv")
    write_manifest_report(manifest_path, rows)
    split_switches, errors, warnings = manifest_summary(rows)
    print(f"[manifest] entries={len(rows)} split_switches={len(split_switches)} manifest_errors={len(errors)} other_warnings={len(warnings)}")
    warning_counts = collections.Counter(row["issue"].split(":", 1)[1] for row in warnings)
    if warning_counts:
        print("[manifest-warning-counts] " + " ".join(
            f"{kind}={count}" for kind, count in sorted(warning_counts.items())))
    print(f"[manifest-report] {manifest_path}")
    for row in errors + warnings:
        detail = f"; detail={row['rule_detail']}" if row["rule_detail"] else ""
        print(f"[manifest-{row['issue'].split(':', 1)[0].lower()}] 0x{row['address']:08X} {row['name']}: {row['issue']}; {row['body']}; refs={row['references']}; words={row['words']}{detail}")
    fatal_rows = generated_fatals(generated)
    fatal_path = os.path.join(report_dir, "generated-fatals.tsv")
    with open(fatal_path, "w", encoding="utf-8") as f:
        f.write("file\tline\tmessage\n")
        for source, lineno, message in fatal_rows:
            f.write(f"{source}\t{lineno}\t{message}\n")
    print(f"[generated-fatals] {len(fatal_rows)}; report={fatal_path}")
    log_errors = []
    if log:
        log_errors, groups = parse_codegen_log(log)
        report = os.path.join(report_dir, "codegen-integrity.md")
        write_codegen_report(report, log, log_errors, groups, fatal_rows)
        jumps = sum(group["count"] for (kind, _), group in groups.items() if kind == "jump@bctr")
        sites = sum(1 for kind, _ in groups if kind == "jump@bctr")
        print(f"[codegen] errors={len(log_errors)} jump_errors={jumps} bctr_sites={sites}; report={report}")
    return not errors and not log_errors


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", default=os.path.join(ROOT, "config", "functions.toml"))
    ap.add_argument("--image", default=DEFAULT_IMAGE, help="flat guest image; file offset is guest address minus 0x82000000")
    ap.add_argument("--generated", default=DEFAULT_GENERATED, help="generated snapshot/output directory")
    ap.add_argument("--codegen-log", help="codegen log; any [error] line makes normal mode exit non-zero")
    ap.add_argument("--report-dir", default=AUTO1)
    ap.add_argument("--selftest", action="store_true", help="run known pre-c214/c214 manifest and log controls")
    ap.add_argument("--selftest-patterns", action="store_true",
                    help="test dispatch-boundary rules with synthetic words and the selected manifest/image")
    args = ap.parse_args()
    try:
        if args.selftest_patterns:
            ok = selftest_patterns(args.manifest, args.image, args.generated)
        elif args.selftest:
            ok = selftest(args.image, args.generated, args.report_dir)
        else:
            ok = analyze(args.manifest, args.image, args.generated, args.codegen_log, args.report_dir)
    except (OSError, ValueError, KeyError, tomllib.TOMLDecodeError) as exc:
        print(f"[check-manifest-integrity] ERROR: {exc}", file=sys.stderr)
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
