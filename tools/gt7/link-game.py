"""Makes a game folder Kyty can load from a PPSA01317 dump that keeps the encrypted SELFs next to
their decrypted ELFs (eboot.bin + eboot.elf, sce_module/libc.prx + libc.elf, ...): every file is
hard-linked (no extra disk space, the dump is left as it is), and eboot.bin and each .prx/.sprx that
has a decrypted .elf sibling link to that ELF instead.

Usage: python link-game.py <dump folder> <new game folder>
"""
import os
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    source, destination = sys.argv[1], sys.argv[2]
    linked = replaced = 0
    for root, _, files in os.walk(source):
        relative = os.path.relpath(root, source)
        out = destination if relative == '.' else os.path.join(destination, relative)
        os.makedirs(out, exist_ok=True)
        names = set(files)
        for name in files:
            if name.endswith('.elf') or name.endswith('.exe'):
                continue
            target = os.path.join(out, name)
            if os.path.exists(target):
                continue
            base, extension = os.path.splitext(name)
            origin = os.path.join(root, name)
            if (name == 'eboot.bin' or extension in ('.prx', '.sprx')) and base + '.elf' in names:
                origin = os.path.join(root, base + '.elf')
                replaced += 1
            os.link(origin, target)
            linked += 1
    print('linked %d files, %d of them to their decrypted ELF' % (linked, replaced))


if __name__ == '__main__':
    main()
