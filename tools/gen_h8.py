#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:Olivier Galibert
#
# ReSys12: C code generator for the H8/300H interpreter (src/h8/h8_ops.inc).
# Derived from MAME src/devices/cpu/h8/h8make.py (BSD-3-Clause, Olivier Galibert).
# Changes from MAME: emits C instead of C++, only the "full" execution mode (one call runs a whole instruction, no
# partial/substate machinery, no bcount checks inside instructions), only one chip type, flattened (no inheritance:
# every enabled opcode of the requested type is emitted, the most derived definition wins).
#
# Usage: gen_h8.py h8.lst h8_ops.inc   (H8/300H only: see CONSTANTS)
import re
import sys

USAGE = "Usage: %s h8.lst h8_ops.inc [o|h|s20|s26|g]\n"


def name_to_type(name):
    types = {"o": 0, "h": 1, "s20": 2, "s26": 3, "g": 4}
    if name not in types:
        sys.stderr.write("Unknown chip type name %s\n" % name)
        sys.exit(1)
    return types[name]


def hexsplit(s):
    return [int(s[i:i + 2], 16) for i in range(0, len(s), 2)]


# m_FOO -> h8.FOO (the core state lives in a single global struct)
RE_MEMBER = re.compile(r"\bm_(\w+)")


# Chip configuration flags that are constant for the H8/300H (H8/3002: advanced mode, H flag present, no EXR/MAC)
CONSTANTS = {
    "m_has_hc": "1",
    "m_has_mac": "0",
    "m_has_exr": "0",
    "m_has_trace": "0",
    "m_mode_advanced": "1",
}
RE_CONST = re.compile(r"\b(" + "|".join(CONSTANTS.keys()) + r")\b")


def c_line(line):
    line = RE_CONST.sub(lambda m: CONSTANTS[m.group(1)], line)
    return RE_MEMBER.sub(r"h8.\1", line)


# Opcodes whose MAME body relies on the device/scheduler: replaced by C bodies.
#  sleep: MAME suspends the cpu on software standby (never armed on the H8/3002 here) or eats cycles until an
#  interrupt is pending. h8.sleep_fetched keeps the prefetch from being repeated while waiting.
CUSTOM_BODIES = {
    "sleep": [
        "\tif(!h8.sleep_fetched) {",
        "\t\th8.NPC = h8.PC & 0xffffff;",
        "\t\th8.PIR = read16i(h8.PC);",
        "\t\th8.PC += 2;",
        "\t\th8.sleep_fetched = 1;",
        "\t}",
        "\tif(!h8.irq_vector) {",
        "\t\th8_eat_all_cycles();",
        "\t\treturn;",
        "\t}",
        "\th8.sleep_fetched = 0;",
        "\tprefetch_done();",
    ],
}


# Special states not emitted: the on-chip DMA controller is not emulated (the firmware does not use it).
SKIP_STATES = {"dma"}


def save_one(f, fname, source, custom=None):
    print("static void %s(void)" % fname, file=f)
    print("{", file=f)
    if custom is not None:
        for line in custom:
            print(line, file=f)
    else:
        for line in source:
            if "eat-all-cycles" in line:
                sys.stderr.write("eat-all-cycles outside a custom body in %s\n" % fname)
                sys.exit(1)
            print(c_line(line), file=f)
    print("}", file=f)
    print("", file=f)


class Hash:
    def __init__(self, premask):
        self.mask = 0x00
        self.enabled = False
        self.premask = premask
        self.d = {}

    def get(self, val, premask):
        if val in self.d:
            h = self.d[val]
            if h.premask != premask:
                sys.stderr.write("Premask conflict\n")
                sys.exit(1)
            return h
        h = Hash(premask)
        self.d[val] = h
        return h

    def set(self, val, opc):
        if val in self.d:
            sys.stderr.write("Collision on %s\n" % opc.description())
            sys.exit(1)
        self.d[val] = opc


class Opcode:
    def __init__(self, val, mask, skip, name, am1, am2, otype, dtype):
        self.name = name
        self.val = hexsplit(val)
        self.mask = hexsplit(mask)
        self.skip = int(skip)
        self.am1 = am1
        self.am2 = am2
        self.source = []
        self.otype = otype
        self.enabled = otype == -1 or (otype == 0 and dtype == 0) or (otype != 0 and dtype >= otype)
        if dtype == 0 and (am1 == "r16l" or am2 == "r16l"):
            self.mask[len(self.mask) - 1] |= 0x08
        if dtype == 0 and (am1 == "r16h" or am2 == "r16h"):
            self.mask[len(self.mask) - 1] |= 0x80
        extra_words = 0
        if (am1 == "abs16" or am2 == "abs16" or am1 == "abs16e" or am1 == "abs22e" or am1 == "abs24e") and self.skip == 0:
            extra_words += 1
        if (am1 == "abs32" or am2 == "abs32") and self.skip == 0:
            extra_words += 2
        if am1 == "imm16" or am1 == "rel16" or am1 == "r16d16h" or am2 == "r16d16h" or am1 == "r32d16h" or am2 == "r32d16h":
            extra_words += 1
        if am1 == "imm32" or am1 == "r32d32hh" or am2 == "r32d32hh":
            extra_words += 2
        self.extra_words = extra_words
        base_offset = len(self.val) // 2 + self.skip
        for i in range(0, extra_words):
            self.source.append("\tm_IR[%d] = read16i(m_PC);" % (i + base_offset))
            self.source.append("\tm_PC += 2;")

    def description(self):
        return "%s %s %s" % (self.name, self.am1, self.am2)

    def add_source_line(self, line):
        self.source.append(line)

    def is_dispatch(self):
        return False

    def function_name(self):
        n = self.name.replace(".", "_")
        if self.am1 != "-":
            n = n + "_" + self.am1
        if self.am2 != "-":
            n = n + "_" + self.am2
        return "op_" + n


class Special:
    def __init__(self, val, name, otype, dtype):
        self.name = name
        self.val = int(val, 16)
        self.enabled = otype == -1 or (otype == 0 and dtype == 0) or (otype != 0 and dtype >= otype)
        self.source = []

    def add_source_line(self, line):
        self.source.append(line)


class Macro:
    def __init__(self, tokens):
        self.name = tokens[1]
        self.params = []
        for i in range(2, len(tokens)):
            self.params.append(tokens[i])
        self.source = []

    def add_source_line(self, line):
        self.source.append(line)

    def apply(self, target, tokens):
        values = []
        if len(self.params) > 1:
            for i in range(0, len(self.params) - 1):
                values.append(tokens[i + 1])
        lval = ""
        for i in range(len(self.params) - 1, len(tokens) - 1):
            if lval != "":
                lval += " "
            lval = lval + tokens[i + 1]
        values.append(lval)
        for i in range(0, len(self.source)):
            line = self.source[i]
            for j in range(0, len(self.params)):
                line = line.replace(self.params[j], values[j])
            target.add_source_line(line)


class DispatchStep:
    def __init__(self, id, pos, opc):
        self.id = id
        self.pos = pos
        self.name = ""
        self.enabled = False
        self.mask = opc.mask[pos - 1]
        for i in range(0, pos):
            self.name += "%02x" % opc.val[i]
        if pos == 2:
            self.skip = opc.skip
        else:
            self.skip = 0

    def is_dispatch(self):
        return True

    def source(self):
        start = self.pos // 2
        end = start + self.skip
        s = []
        for i in range(start, end + 1):
            s.append("\tm_IR[%d] = read16i(m_PC);" % i)
            s.append("\tm_PC += 2;")
        s.append("\tm_inst_state = 0x%x0000 | m_IR[%d];" % (self.id, end))
        return s


class OpcodeList:
    def __init__(self, fname, dtype):
        self.opcode_info = []
        self.dispatch_info = []
        self.states_info = []
        self.dispatch = {}
        self.macros = {}
        try:
            f = open(fname, "r")
        except Exception:
            err = sys.exc_info()[1]
            sys.stderr.write("Cannot read opcodes file %s [%s]\n" % (fname, err))
            sys.exit(1)

        inf = None
        for line in f:
            if line.startswith("#"):
                continue
            line = line.rstrip()
            if not line:
                continue
            if line.startswith(" ") or line.startswith("\t"):
                if inf is not None:
                    tokens = line.split()
                    if tokens[0] in self.macros:
                        self.macros[tokens[0]].apply(inf, tokens)
                    else:
                        inf.add_source_line(line)
            else:
                tokens = line.split()
                if tokens[0] == "macro":
                    inf = Macro(tokens)
                    self.macros[inf.name] = inf
                elif len(tokens) == 2 or len(tokens) == 3:
                    otype = name_to_type(tokens[2]) if len(tokens) >= 3 else -1
                    inf = Special(tokens[0], tokens[1], otype, dtype)
                    self.states_info.append(inf)
                else:
                    otype = name_to_type(tokens[6]) if len(tokens) >= 7 else -1
                    if otype == -1 or dtype == 0 or (otype != 0 and dtype != 0):
                        inf = Opcode(tokens[0], tokens[1], tokens[2], tokens[3], tokens[4], tokens[5], otype, dtype)
                        self.opcode_info.append(inf)
                    else:
                        inf = None
        f.close()

    def get(self, i):
        if i in self.dispatch:
            return self.dispatch[i]
        h = Hash(0)
        self.dispatch[i] = h
        return h

    def build_dispatch(self):
        for opc in self.opcode_info:
            for i in range(0, len(opc.val)):
                v = opc.val[i]
                if i == 0:
                    h = self.get(0)
                if opc.enabled:
                    h.mask = h.mask | opc.mask[i]
                    h.enabled = True
                if (i & 1) == 0:
                    h = h.get(v, opc.mask[i])
                elif i == len(opc.val) - 1:
                    if opc.enabled:
                        h.set(v, opc)
                else:
                    if v in h.d:
                        d = h.d[v]
                        if not d.is_dispatch():
                            sys.stderr.write("Collision on %s\n" % opc.description())
                            sys.exit(1)
                        if opc.enabled:
                            d.enabled = True
                        h = self.get(d.id)
                    else:
                        d = DispatchStep(len(self.dispatch_info) + 2, i + 1, opc)
                        self.dispatch_info.append(d)
                        if opc.enabled:
                            d.enabled = True
                        h.set(v, d)
                        h = self.get(d.id)

    def save_opcodes(self, f):
        for opc in self.opcode_info:
            if opc.enabled:
                save_one(f, opc.function_name(), opc.source, CUSTOM_BODIES.get(opc.name))
        for sta in self.states_info:
            if sta.enabled and sta.name not in SKIP_STATES:
                save_one(f, "state_" + sta.name, sta.source)

    def save_dispatch(self, f):
        for dsp in self.dispatch_info:
            if dsp.enabled:
                save_one(f, "dispatch_" + dsp.name, dsp.source())

    def save_exec(self, f):
        print("static void h8_exec_full(void)", file=f)
        print("{", file=f)
        print("\tswitch(h8.inst_state >> 16) {", file=f)
        for i in range(0, len(self.dispatch_info) + 2):
            if i == 1:
                print("\tcase 0x01: {", file=f)
                print("\t\tswitch(h8.inst_state & 0xffff) {", file=f)
                for sta in self.states_info:
                    if sta.enabled and sta.name not in SKIP_STATES:
                        print("\t\tcase 0x%02x: state_%s(); break;" % (sta.val & 0xffff, sta.name), file=f)
                print("\t\tdefault: h8_unhandled_state(); break;", file=f)
                print("\t\t}", file=f)
                print("\t\tbreak;", file=f)
                print("\t}", file=f)
            else:
                if i == 0 or self.dispatch_info[i - 2].enabled:
                    print("\tcase 0x%02x: {" % i, file=f)
                    h = self.get(i)
                    print("\t\tswitch((h8.inst_state >> 8) & 0x%02x) {" % h.mask, file=f)
                    for val, h2 in sorted(h.d.items()):
                        if h2.enabled:
                            fmask = h2.premask | (h.mask ^ 0xff)
                            c = ""
                            s = 0
                            while s < 0x100:
                                c += "case 0x%02x: " % (val | s)
                                s += 1
                                while s & fmask:
                                    s += s & fmask
                            print("\t\t%s{" % c, file=f)
                            if h2.mask == 0x00:
                                n = h2.d[0]
                                if n.is_dispatch():
                                    print("\t\t\tdispatch_%s();" % n.name, file=f)
                                else:
                                    print("\t\t\t%s();" % n.function_name(), file=f)
                                print("\t\t\tbreak;", file=f)
                            else:
                                print("\t\t\tswitch(h8.inst_state & 0x%02x) {" % h2.mask, file=f)
                                if i == 0:
                                    mpos = 1
                                else:
                                    mpos = self.dispatch_info[i - 2].pos + 1
                                for val2, n in sorted(h2.d.items()):
                                    if n.enabled:
                                        fmask = h2.mask ^ 0xff
                                        if n.is_dispatch():
                                            fmask = fmask | n.mask
                                        else:
                                            fmask = fmask | n.mask[mpos]
                                        c = ""
                                        s = 0
                                        while s < 0x100:
                                            c += "case 0x%02x: " % (val2 | s)
                                            s += 1
                                            while s & fmask:
                                                s += s & fmask
                                        if n.is_dispatch():
                                            print("\t\t\t%sdispatch_%s(); break;" % (c, n.name), file=f)
                                        else:
                                            print("\t\t\t%s%s(); break;" % (c, n.function_name()), file=f)
                                print("\t\t\tdefault: illegal(); break;", file=f)
                                print("\t\t\t}", file=f)
                                print("\t\t\tbreak;", file=f)
                            print("\t\t}", file=f)
                    print("\t\tdefault: illegal(); break;", file=f)
                    print("\t\t}", file=f)
                    print("\t\tbreak;", file=f)
                    print("\t}", file=f)
        print("\tdefault: illegal(); break;", file=f)
        print("\t}", file=f)
        print("}", file=f)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(USAGE % argv[0])
        return 1
    dtype = name_to_type("h")
    opcodes = OpcodeList(argv[1], dtype)
    opcodes.build_dispatch()
    try:
        f = open(argv[2], "w", newline="\n")
    except Exception:
        err = sys.exc_info()[1]
        sys.stderr.write("cannot write file %s [%s]\n" % (argv[2], err))
        sys.exit(1)
    print("// license:BSD-3-Clause", file=f)
    print("// copyright-holders:Olivier Galibert", file=f)
    print("// ReSys12: generated by tools/gen_h8.py from MAME src/devices/cpu/h8/h8.lst. Do not edit.", file=f)
    print("// Included by src/h8/h8.c (H8/300H, full-instruction execution).", file=f)
    print("", file=f)
    opcodes.save_opcodes(f)
    opcodes.save_dispatch(f)
    opcodes.save_exec(f)
    f.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
