"""Samples named threads of a running kyty_emulator: each sample suspends the thread, reads its
instruction pointer and scans the top of its stack for return addresses into the emulator, then
resumes it. The samples are symbolized with llvm-symbolizer and the emulator's PDB (build it with
build-gt7-prof.bat).

Usage: python sample-threads.py [--seconds 30] [--rate 1000] [--threads Thread_Gpu,GpuRecord]
                                [--out samples.txt]
Reports, per thread: self time by function (where the thread was), and inclusive time by function
(functions found on its stack; a stack scan, so stale return addresses add some noise).
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

TH32CS_SNAPTHREAD = 0x4
THREAD_ALL = 0x1FFFFF
PROCESS_ALL = 0x1F0FFF
CONTEXT_AMD64 = 0x100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG),
                ('dwFlags', wt.DWORD)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [('lpBaseOfDll', ctypes.c_void_p), ('SizeOfImage', wt.DWORD), ('EntryPoint', ctypes.c_void_p)]


# CONTEXT is 1232 bytes and must be 16-byte aligned; the offsets used are fixed by the x64 ABI.
CONTEXT_SIZE = 1232
OFF_FLAGS, OFF_RSP, OFF_RIP = 0x30, 0x98, 0xF8

k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
psapi = ctypes.WinDLL('psapi', use_last_error=True)
psapi.GetModuleInformation.argtypes = [wt.HANDLE, wt.HMODULE, ctypes.POINTER(MODULEINFO), wt.DWORD]
psapi.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD,
                                     ctypes.POINTER(wt.DWORD)]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]


def find_pid():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq kyty_emulator.exe', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower().startswith('kyty_emulator'):
            return int(parts[1])
    raise SystemExit('kyty_emulator is not running')


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    result = []
    while ok:
        if entry.th32OwnerProcessID == pid:
            result.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return result


def main_module(process):
    modules = (wt.HMODULE * 1)()
    needed = wt.DWORD()
    psapi.EnumProcessModules(process, modules, ctypes.sizeof(modules), ctypes.byref(needed))
    info = MODULEINFO()
    psapi.GetModuleInformation(process, modules[0], ctypes.byref(info), ctypes.sizeof(info))
    name = ctypes.create_unicode_buffer(1024)
    psapi.GetModuleFileNameExW(process, modules[0], name, 1024)
    return info.lpBaseOfDll, info.SizeOfImage, name.value


def module_ranges(process):
    modules = (wt.HMODULE * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModules(process, modules, ctypes.sizeof(modules), ctypes.byref(needed))
    ranges = []
    for i in range(min(needed.value // ctypes.sizeof(wt.HMODULE), 1024)):
        info = MODULEINFO()
        psapi.GetModuleInformation(process, modules[i], ctypes.byref(info), ctypes.sizeof(info))
        name = ctypes.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(process, modules[i], name, 1024)
        ranges.append((info.lpBaseOfDll, info.lpBaseOfDll + info.SizeOfImage, os.path.basename(name.value)))
    return ranges


def read(process, address, size):
    buffer = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(process, address, buffer, size, ctypes.byref(got)):
        return b''
    return buffer.raw[:got.value]


def exports_of(process, base):
    """A loaded module's exports, sorted by address, read from its image in the process."""
    header = read(process, base, 4096)
    if len(header) < 0x200:
        return []
    pe = struct.unpack_from('<I', header, 0x3C)[0]
    rva, size = struct.unpack_from('<II', header, pe + 24 + 112)
    if rva == 0:
        return []
    directory = read(process, base + rva, 40)
    if len(directory) < 40:
        return []
    count_functions, count_names, functions, names, ordinals = struct.unpack_from('<IIIII', directory, 20)
    function_rvas = struct.unpack('<%dI' % count_functions, read(process, base + functions, 4 * count_functions))
    name_rvas = struct.unpack('<%dI' % count_names, read(process, base + names, 4 * count_names))
    name_ordinals = struct.unpack('<%dH' % count_names, read(process, base + ordinals, 2 * count_names))
    result = []
    for name_rva, ordinal in zip(name_rvas, name_ordinals):
        text = read(process, base + name_rva, 128).split(b'\0', 1)[0].decode('ascii', 'replace')
        result.append((base + function_rvas[ordinal], text))
    result.sort()
    return result


def preferred_base(path):
    with open(path, 'rb') as f:
        data = f.read(4096)
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    return struct.unpack_from('<Q', data, pe + 24 + 24)[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--rate', type=float, default=1000)
    parser.add_argument('--threads', default='Thread_Gpu,GpuRecord')
    parser.add_argument('--depth', type=int, default=24)
    parser.add_argument('--out', default=None)
    parser.add_argument('--raw', default=None, help='JSON file for every sample\'s leaf and callers')
    parser.add_argument('--guest-exact', action='store_true',
                        help='guest code by 16-byte line instead of 4 KB page')
    args = parser.parse_args()
    wanted = set(args.threads.split(','))

    pid = find_pid()
    process = k32.OpenProcess(PROCESS_ALL, False, pid)
    base, size, exe = main_module(process)
    end = base + size
    targets = {}
    for tid in threads_of(pid):
        handle = k32.OpenThread(THREAD_ALL, False, tid)
        if not handle:
            continue
        desc = ctypes.c_wchar_p()
        name = ''
        if k32.GetThreadDescription(handle, ctypes.byref(desc)) >= 0 and desc.value:
            name = desc.value
            k32.LocalFree(desc)
        if name in wanted:
            targets[handle] = name + ('' if name not in [v for v in targets.values()] else '#%d' % tid)
        else:
            k32.CloseHandle(handle)
    if not targets:
        raise SystemExit('none of %s found' % sorted(wanted))
    print('sampling', sorted(targets.values()), 'in', exe, file=sys.stderr)

    raw = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
    ctx_addr = (ctypes.addressof(raw) + 15) & ~15
    stack = ctypes.create_string_buffer(8 * 512)
    got = ctypes.c_size_t()
    samples = {name: [] for name in targets.values()}
    period = 1.0 / args.rate
    deadline = time.perf_counter() + args.seconds
    next_time = time.perf_counter()
    while time.perf_counter() < deadline:
        for handle, name in targets.items():
            if k32.SuspendThread(handle) == 0xFFFFFFFF:
                continue
            try:
                ctypes.memset(ctx_addr, 0, CONTEXT_SIZE)
                ctypes.c_uint32.from_address(ctx_addr + OFF_FLAGS).value = CONTEXT_CONTROL | CONTEXT_INTEGER
                if not k32.GetThreadContext(handle, ctx_addr):
                    continue
                rip = ctypes.c_uint64.from_address(ctx_addr + OFF_RIP).value
                rsp = ctypes.c_uint64.from_address(ctx_addr + OFF_RSP).value
                k32.ReadProcessMemory(process, rsp, stack, ctypes.sizeof(stack), ctypes.byref(got))
            finally:
                k32.ResumeThread(handle)
            callers = []
            for (value,) in struct.iter_unpack('<Q', stack.raw[:got.value - got.value % 8]):
                if base <= value < end:
                    callers.append(value)
                    if len(callers) >= args.depth:
                        break
            samples[name].append((rip, tuple(callers)))
        next_time += period
        delay = next_time - time.perf_counter()
        if delay > 0:
            time.sleep(delay)

    # Symbolize every address once.
    modules = module_ranges(process)
    prefer = preferred_base(exe)
    addresses = set()
    for entries in samples.values():
        for rip, callers in entries:
            addresses.add(rip)
            addresses.update(callers)
    inside = sorted(a for a in addresses if base <= a < end)
    symbolizer = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-symbolizer.exe'
    names = {}
    if inside:
        # Callers are return addresses: look up the call instruction before them.
        query = '\n'.join('0x%x' % (a - base + prefer) for a in inside)
        out = subprocess.run([symbolizer, '--obj=' + exe, '--functions=short', '--no-inlines', '--output-style=GNU'],
                             input=query, capture_output=True, text=True).stdout.splitlines()
        for i, a in enumerate(inside):
            names[a] = out[2 * i] if 2 * i < len(out) else '?'
    export_cache = {}

    def outside_name(address):
        module = next(((lo, m) for lo, hi, m in modules if lo <= address < hi), None)
        if module is None:
            return '[guest code @0x%x]' % (address & ~0xf if args.guest_exact else address & ~0xfff)
        lo, m = module
        if m.lower().startswith(('nvoglv', 'nvgpucomp')):
            return '[%s]' % m
        if lo not in export_cache:
            export_cache[lo] = exports_of(process, lo)
        best = None
        for start, text in export_cache[lo]:
            if start > address:
                break
            best = text
        return '[%s!%s]' % (m, best or '?')

    lines = []
    for name, entries in samples.items():
        total = len(entries)
        self_count = collections.Counter()
        incl = collections.Counter()
        # Outside the emulator, by system function and the first emulator function that called it.
        outside_by_caller = collections.Counter()
        outside = 0
        for rip, callers in entries:
            leaf = names.get(rip)
            if leaf is None:
                outside += 1
                leaf = outside_name(rip)
                caller = next((names[c] for c in callers if c in names), '?')
                outside_by_caller['%s  <-  %s' % (leaf, caller)] += 1
            self_count[leaf] += 1
            seen = {leaf}
            for c in callers:
                seen.add(names.get(c, '?'))
            for s in seen:
                incl[s] += 1
        lines.append('== %s: %d samples (%.0f%% outside the emulator)' % (name, total, 100.0 * outside / max(total, 1)))
        lines.append('-- self')
        for fn, n in self_count.most_common(40):
            lines.append('%6.2f%%  %s' % (100.0 * n / total, fn))
        lines.append('-- outside the emulator, by first emulator caller (stack scan)')
        for fn, n in outside_by_caller.most_common(30):
            lines.append('%6.2f%%  %s' % (100.0 * n / total, fn))
        lines.append('-- inclusive (stack scan)')
        for fn, n in incl.most_common(60):
            lines.append('%6.2f%%  %s' % (100.0 * n / total, fn))
    if args.raw:
        # Every sample's leaf and caller chain, by name, for further digging.
        import json
        dump = {name: [[names.get(rip) or outside_name(rip)] + [names.get(c, '?') for c in callers]
                       for rip, callers in entries] for name, entries in samples.items()}
        with open(args.raw, 'w', encoding='utf-8') as f:
            json.dump(dump, f)
    text = '\n'.join(lines)
    print(text)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text + '\n')


if __name__ == '__main__':
    main()
