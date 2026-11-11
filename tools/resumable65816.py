#!/usr/bin/env python3
# Copyright (c) 2026 Magnominus
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""resumable65816.py [w65816.inc] [w65816r.inc]: the 65816 for a processor that can stop at any clock
step, such as the SA-1, made from the 65816 the S-CPU uses.

Every instruction becomes a macro. Its bus accesses and idle cycles become READ, WRITE and IDLE
(and the like), which the includer defines as calls into its state machine, and everything an
instruction holds between its cycles is in R. The algorithms and the address helpers come across
unchanged.
"""
import re
import sys

source = sys.argv[1] if len(sys.argv) > 1 else 'src/w65816.inc'
target = sys.argv[2] if len(sys.argv) > 2 else 'src/w65816r.inc'
text = open(source).read()


def call_end(s, start):
    """The index just past the ')' that closes the call whose '(' is at start."""
    depth = 0
    for i in range(start, len(s)):
        if s[i] == '(':
            depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0:
                return i + 1
    raise ValueError(s)


def split_args(s):
    args, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return args


reads = {
    'fetch': None,
    'read': lambda a: a[0],
    'readDirect': lambda a: 'addressDirect(%s)' % a[0],
    'readDirectX': lambda a: 'addressDirectX(%s, %s)' % (a[0], a[1]),
    'readDirectN': lambda a: '(u16) (D.w + (%s))' % a[0],
    'readBank': lambda a: '(B << 16) + (%s)' % a[0],
    'readStack': lambda a: '(u16) (S.w + (%s))' % a[0],
    'pull': None,
    'pullN': None,
}
writes = {
    'write': lambda a: a[0],
    'writeDirect': lambda a: 'addressDirect(%s)' % a[0],
    'writeBank': lambda a: '(B << 16) + (%s)' % a[0],
    'writeStack': lambda a: '(u16) (S.w + (%s))' % a[0],
}
idles = {'idle': 'IDLE()', 'idleIRQ': 'IDLE_IRQ()', 'idleJump': 'IDLE_JUMP()', 'idleBranch': 'IDLE_BRANCH()'}


def read_statement(target_expr, name, args):
    if name == 'fetch':
        return 'FETCH(%s);' % target_expr
    if name == 'pull':
        return 'PULL(%s);' % target_expr
    if name == 'pullN':
        return 'PULL_N(%s);' % target_expr
    return 'READ(%s, %s);' % (target_expr, reads[name](args))


def convert_statement(stmt):
    """One C statement (without its ';') of an instruction body, as the resumable form."""
    s = stmt.strip()
    if not s:
        return ''
    # target = call(...)
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_.>\-\*\(\)&]*?)\s*=\s*([A-Za-z0-9]+)\((.*)\)$', s)
    if m and m.group(2) in reads and call_end(s, s.index(m.group(2) + '(') + len(m.group(2))) == len(s):
        return read_statement(m.group(1), m.group(2), split_args(m.group(3)))
    m = re.match(r'^([A-Za-z0-9]+)\((.*)\)$', s)
    if m and call_end(s, len(m.group(1))) == len(s):
        name, args = m.group(1), split_args(m.group(2))
        if name in reads:
            return read_statement('R.dr', name, args)
        if name in writes:
            return 'WRITE(%s, %s);' % (writes[name](args), args[-1])
        if name == 'push':
            return 'PUSH(%s);' % args[0]
        if name == 'pushN':
            return 'PUSH_N(%s);' % args[0]
        if name in idles:
            return idles[name] + ';'
        if name == 'idle2':
            return 'if (D.l) { IDLE(); }'
        if name == 'idle4':
            return 'if (!XF || (u16) (%s) >> 8 != (u16) (%s) >> 8) { IDLE(); }' % (args[0], args[1])
        if name == 'idle6':
            return 'if (EF && PC.h != (u16) (%s) >> 8) { IDLE(); }' % args[0]
    # A read inside an expression, such as setP(pull()): the read first, then the expression with its
    # value.
    for name in reads:
        m = re.search(r'\b%s\(' % name, s)
        if m:
            end = call_end(s, m.end() - 1)
            args = split_args(s[m.end():end - 1])
            return read_statement('R.dr', name, args) + ' ' + convert_statement(s[:m.start()] + 'R.dr' + s[end:])
    for name in list(writes) + ['push', 'pushN', 'idle', 'idle2', 'idle4', 'idle6', 'idleIRQ']:
        if re.search(r'\b%s\(' % name, s):
            raise ValueError('a bus access in an expression: ' + s)
    return s + ';'


def split_statements(line):
    """A line of a body: its statements, braces and keywords kept."""
    out, depth, cur = [], 0, ''
    for ch in line:
        cur += ch
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        elif ch == ';' and depth == 0:
            out.append(cur)
            cur = ''
    if cur.strip():
        out.append(cur)
    return out


def convert_line(line):
    indent = re.match(r'^\s*', line).group(0)
    body = line.strip()
    # if (cond) statement;   if (cond) {   } else {   }
    m = re.match(r'^(if \(.*\)) (?!\{)(.*;)$', body)
    if m:
        cond = m.group(1)
        inner = ' '.join(convert_statement(st.rstrip(';')) for st in split_statements(m.group(2)))
        return indent + cond + ' { ' + inner + ' }'
    if body.endswith('{') or body in ('}', '} else {') or body.startswith('} else'):
        return line
    return indent + ' '.join(convert_statement(st.rstrip(';')) for st in split_statements(body))


functions = re.findall(r'static void instruction(\w+)\((.*?)\) \{\n(.*?)\n\}\n', text, re.S)
macros = []
for name, params, body in functions:
    names = [p.split()[-1].lstrip('*') for p in params.split(',')] if params != 'void' else []
    lines = [convert_line(l) for l in body.split('\n')]
    # Macro arguments are expressions such as NF == 0 or &A: every use is put in parentheses.
    for p in names:
        lines = [re.sub(r'(?<![.\w])%s\b' % p, '(%s)' % p, l) for l in lines]
    # Parameters given as pointers are used through them; as macros they are given as &X.
    out = '\\\n'.join(l.replace('\t', '\t') for l in lines)
    macros.append('#define I_%s(%s) \\\n%s' % (name, ', '.join(names), out))

# The dispatch: each opcode's macro, with M and X picked inside.
dispatch = re.search(r'static void instruction\(void\) \{\n\tswitch \(fetch\(\)\) \{\n(.*?)\n\t\}\n\}', text, re.S).group(1)
cases = []
for line in dispatch.split('\n'):
    m = re.match(r'\tcase (0x[0-9a-f]+): (.*) return;$', line)
    code = m.group(2)
    code = re.sub(r'instruction(\w+)\(', r'I_\1(', code)
    m2 = re.match(r'if \((MF|XF)\) (I_.*?); else (I_.*?);$', code)
    if m2:
        code = 'if (%s) { %s } else { %s }' % (m2.group(1), m2.group(2), m2.group(3))
    else:
        code = code.rstrip(';')
    cases.append('\tcase %s: %s goto done;' % (m.group(1), code))

# What comes across unchanged: the register names, the algorithms and the address arithmetic.
head = text[:text.index('// An immediate two-cycle opcode')]
head = head.replace('// Only the SA-1 does anything on these.\n#define idleBranch()\n#define idleJump()\n', '')
head = re.sub(r'static void w65816Power\(void\) \{.*?\n\}\n', '', head, flags=re.S)
algorithms = text[text.index('static u8 algorithmADC8'):text.index('static void instructionImmediateRead8')]

out = ['/* Generated by tools/resumable65816.py from w65816.inc. */', '',
       '// The 65816 for a processor that can stop at any clock step. The includer defines R, the',
       '// statements READ, WRITE, FETCH, PULL, PULL_N, PUSH, PUSH_N, IDLE, IDLE_IRQ, IDLE_JUMP and',
       '// IDLE_BRANCH, and lastCycle(), and puts W65816_DISPATCH where an opcode in R.op starts.', '',
       head.split('\n', 5)[5] if head.startswith('/*') else head,
       '''static inline u32 addressDirect(u32 address) {
	if (EF && !D.l) {
		return D.w | (u8) address;
	}
	return (u16) (D.w + address);
}

// (direct,X) wraps the high byte within the page in emulation mode when D's low byte isn't zero.
static inline u32 addressDirectX(u32 address, u32 offset) {
	if (EF && D.l) {
		return ((D.w + address) & 0xffff00) | (u8) (D.w + address + offset);
	}
	return addressDirect(address + offset);
}
''', algorithms]
out += macros
out += ['', '#define W65816_DISPATCH \\', ' \\\n'.join(cases)]
open(target, 'w').write('\n'.join(out) + '\n')
print('%d instructions, %d opcodes' % (len(macros), len(cases)))
