"""Sends key presses to the kyty_emulator window (PostMessage WM_KEYDOWN/WM_KEYUP: no focus needed)
and saves window screenshots. Default Kyty keymap: arrows = D-pad, J = Cross, L = Circle, I = Triangle,
K = Square, C = R2, Z = L2, Return = Options.

Usage: python kyty-input.py press <key> [hold_ms] | seq <key> <key> ... | hold <key> <ms> | shot <file>
Keys: up down left right cross circle triangle square r2 l2 options (or a single letter)."""
import ctypes
import subprocess
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL('user32', use_last_error=True)
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK = {'up': 0x26, 'down': 0x28, 'left': 0x25, 'right': 0x27, 'cross': ord('J'), 'circle': ord('L'), 'triangle': ord('I'),
      'square': ord('K'), 'r2': ord('C'), 'l2': ord('Z'), 'options': 0x0D}
EXTENDED = {0x26, 0x28, 0x25, 0x27}


def window():
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        length = user32.GetWindowTextLengthW(hwnd)
        if length:
            text = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, text, length + 1)
            if 'Gran Turismo' in text.value and 'fps' in text.value:
                found.append(hwnd)
        return True
    user32.EnumWindows(each, 0)
    if not found:
        raise SystemExit('kyty_emulator window not found')
    return found[0]


def key_message(hwnd, vk, down):
    scan = user32.MapVirtualKeyW(vk, 0)
    lparam = 1 | (scan << 16)
    if vk in EXTENDED:
        lparam |= 1 << 24
    if not down:
        lparam |= (1 << 30) | (1 << 31)
    user32.PostMessageW(hwnd, WM_KEYDOWN if down else WM_KEYUP, vk, lparam)


def press(name, hold_ms=150):
    hwnd = window()
    vk = VK.get(name.lower(), ord(name.upper()[0]))
    key_message(hwnd, vk, True)
    time.sleep(hold_ms / 1000)
    key_message(hwnd, vk, False)


def title():
    hwnd = window()
    text = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, text, 512)
    return text.value


def main():
    command = sys.argv[1]
    if command == 'press':
        press(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 150)
    elif command == 'seq':
        for name in sys.argv[2:]:
            press(name)
            time.sleep(1.5)
    elif command == 'hold':
        press(sys.argv[2], int(sys.argv[3]))
    elif command == 'shot':
        subprocess.run(['powershell', '-ExecutionPolicy', 'Bypass', '-File', __file__.replace('kyty-input.py', 'shot.ps1'), '-Out', sys.argv[2]], check=True)
    elif command == 'title':
        print(title())


if __name__ == '__main__':
    main()
