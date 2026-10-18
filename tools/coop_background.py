"""Run disposable test copies on an isolated Windows desktop (never switch to it).

The game explicitly shows its window on startup, so Start-Process -WindowStyle
Hidden alone is insufficient. Keep this supervisor running while using the UDP
control ports; a stop file or Ctrl+C terminates only the processes it created.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import subprocess
import time


class Startup(C.Structure):
    _fields_ = [("cb", W.DWORD), ("reserved", W.LPWSTR), ("desktop", W.LPWSTR),
                ("title", W.LPWSTR), ("x", W.DWORD), ("y", W.DWORD),
                ("width", W.DWORD), ("height", W.DWORD), ("charsx", W.DWORD),
                ("charsy", W.DWORD), ("fill", W.DWORD), ("flags", W.DWORD),
                ("show", W.WORD), ("reserved_size", W.WORD), ("reserved2", C.c_void_p),
                ("stdin", W.HANDLE), ("stdout", W.HANDLE), ("stderr", W.HANDLE)]


class Process(C.Structure):
    _fields_ = [("process", W.HANDLE), ("thread", W.HANDLE), ("pid", W.DWORD), ("tid", W.DWORD)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("--ports", type=int, nargs="+", default=[45703, 45704, 45705])
    p.add_argument("--seconds", type=int, default=1800)
    args = p.parse_args()
    directory = args.directory.resolve()
    stop = directory / "stop-tests"
    if stop.exists():
        raise SystemExit("Remove the old stop-tests file before launching new tests")
    user = C.WinDLL("user32", use_last_error=True)
    kernel = C.WinDLL("kernel32", use_last_error=True)
    user.CreateDesktopW.argtypes = [W.LPCWSTR, C.c_void_p, C.c_void_p, W.DWORD, W.DWORD, C.c_void_p]
    user.CreateDesktopW.restype = W.HANDLE
    user.CloseDesktop.argtypes = [W.HANDLE]
    kernel.CreateProcessW.argtypes = [W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p, W.BOOL,
                                     W.DWORD, C.c_void_p, W.LPCWSTR, C.POINTER(Startup), C.POINTER(Process)]
    kernel.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
    kernel.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
    kernel.CloseHandle.argtypes = [W.HANDLE]
    desktop_name = "BlackCoopTest-" + str(time.time_ns())
    desktop = user.CreateDesktopW(desktop_name, None, None, 0, 0x01FF, None)
    if not desktop:
        raise C.WinError(C.get_last_error())
    processes = []
    try:
        for index, port in enumerate(args.ports, 3):
            command = [str(directory / "BLACK.exe"), "--mute", "--instance", str(index),
                       "--coop-control", str(port)]
            si = Startup()
            si.cb, si.desktop, si.flags, si.show = C.sizeof(si), desktop_name, 1, 0
            pi = Process()
            if not kernel.CreateProcessW(command[0], C.create_unicode_buffer(subprocess.list2cmdline(command)),
                                         None, None, False, 0, None, str(directory), C.byref(si), C.byref(pi)):
                raise C.WinError(C.get_last_error())
            kernel.CloseHandle(pi.thread)
            processes.append((pi.process, pi.pid, port))
            print(json.dumps({"pid": pi.pid, "control": port, "isolated_desktop": True}), flush=True)
        deadline = time.monotonic() + args.seconds
        exited = set()
        while time.monotonic() < deadline and not stop.exists():
            for handle, pid, port in processes:
                code = W.DWORD()
                kernel.GetExitCodeProcess(handle, C.byref(code))
                if code.value != 259 and pid not in exited:
                    exited.add(pid)
                    print(json.dumps({"pid": pid, "control": port, "exit": f"0x{code.value:08X}"}), flush=True)
            if len(exited) == len(processes):
                break
            time.sleep(0.5)
    finally:
        for handle, pid, _ in processes:
            code = W.DWORD()
            kernel.GetExitCodeProcess(handle, C.byref(code))
            if code.value == 259:
                kernel.TerminateProcess(handle, 0)
            kernel.CloseHandle(handle)
        user.CloseDesktop(desktop)


if __name__ == "__main__":
    main()
