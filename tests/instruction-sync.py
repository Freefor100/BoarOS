#!/usr/bin/env python3
"""Check direct-call reachability of fence.i in the actual RISC-V objects.

This is an instruction-presence gate, not a proof of branch ordering or hardware
cache behavior; real executable-mapping tests supply those consumer checks.
"""
import re
import subprocess
import sys

HEADER = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
CALL = re.compile(r'\bR_RISCV_CALL(?:_PLT)?\s+(\S+)')
FENCE = re.compile(r'^\s*[0-9a-f]+:\s+[0-9a-f]+\s+fence\.i(?:\s|$)')


def parse_disassembly(output):
    graph = {}
    current = None
    for line in output.splitlines():
        if line.startswith('Disassembly of section '):
            current = None
        header = HEADER.match(line)
        if header and not header[1].startswith('.'):
            current = header[1]
            graph.setdefault(current, [False, set()])
        if current is None:
            continue
        if FENCE.match(line):
            graph[current][0] = True
        call = CALL.search(line)
        if call:
            graph[current][1].add(call[1])
    return graph


def reaches_fence(graph, entry):
    pending, visited = [entry], set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        node = graph.get(name)
        if node is None:
            continue
        if node[0]:
            return True
        pending.extend(node[1] - visited)
    return False


def main():
    objdump, path, symbol = sys.argv[1:]
    output = subprocess.run([objdump, '-dr', path], check=True,
                            capture_output=True, text=True).stdout
    if not reaches_fence(parse_disassembly(output), symbol):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
