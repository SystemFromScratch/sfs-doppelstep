#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the native_sim build of Doppelstep and expose its DYNAMIXEL bus.

The native_sim program puts the DYNAMIXEL bus on a pseudoterminal whose name
(/dev/pts/N) changes at every start. This tool gives it a fixed name and, when
the host program (ROS 2, DynamixelSDK) runs elsewhere, carries it over TCP.

  run      start zephyr.exe, link its bus PTY to a fixed path (default
           build-native/ttyDXL), optionally serve it on a TCP port
  connect  on the machine of the host program: make a local PTY (default
           /tmp/ttyDXL) connected to the TCP port of "run"

  python3 tools/sim/run_native.py run [--tcp 5555] [-- <zephyr.exe options>]
  python3 tools/sim/run_native.py connect --host localhost --port 5555

Only the Python standard library is used (Linux: pty, termios).
"""

import argparse
import os
import re
import select
import signal
import socket
import subprocess
import sys
import threading
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_EXE = os.path.join(ROOT, "build-native", "zephyr", "zephyr.exe")
BUS_PTY = re.compile(r"uart_1 connected to pseudotty: (\S+)")


def link(target, path):
    """Atomically point the symbolic link `path` at `target`."""
    tmp = path + ".tmp"
    if os.path.lexists(tmp):
        os.unlink(tmp)
    os.symlink(target, tmp)
    os.replace(tmp, path)
    print(f"[run_native] DYNAMIXEL bus: {path} -> {target}", flush=True)


def open_raw(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    return fd


def pump(fd, sock):
    """Copy bytes both ways between a PTY fd and a socket until the socket closes."""
    while True:
        ready, _, _ = select.select([fd, sock], [], [])
        if sock in ready:
            data = sock.recv(4096)
            if not data:
                return
            os.write(fd, data)
        if fd in ready:
            try:
                data = os.read(fd, 4096)
            except OSError:  # EIO: no process on the other side of the PTY
                return
            if data:
                sock.sendall(data)


def serve_tcp(pty_path, port):
    """One TCP client at a time on `port`, connected to the bus PTY."""
    srv = socket.create_server(("0.0.0.0", port), reuse_port=False)
    print(f"[run_native] TCP bridge on port {port}", flush=True)
    while True:
        sock, peer = srv.accept()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"[run_native] TCP client {peer[0]}:{peer[1]}", flush=True)
        fd = open_raw(pty_path)
        try:
            pump(fd, sock)
        finally:
            os.close(fd)
            sock.close()
            print("[run_native] TCP client gone", flush=True)


def cmd_run(args):
    exe = os.path.abspath(args.exe)
    if not os.access(exe, os.X_OK):
        sys.exit(f"{exe} not found: build with the task \"Zephyr: Build (native_sim)\"")
    workdir = os.path.dirname(os.path.dirname(exe))  # flash.bin (NVS) stays in the build directory
    path = os.path.abspath(args.link)

    proc = subprocess.Popen([exe] + args.zephyr_args, cwd=workdir, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    signal.signal(signal.SIGTERM, lambda *_: proc.terminate())
    try:
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            match = BUS_PTY.search(line)
            if match:
                link(match.group(1), path)
                if args.tcp:
                    threading.Thread(target=serve_tcp, args=(match.group(1), args.tcp),
                                     daemon=True).start()
    except KeyboardInterrupt:
        proc.terminate()
    finally:
        proc.wait()
        if os.path.islink(path):
            os.unlink(path)
    return proc.returncode


def cmd_connect(args):
    master, slave = os.openpty()
    tty.setraw(slave)
    tty.setraw(master)
    path = os.path.abspath(args.link)
    link(os.ttyname(slave), path)
    try:
        while True:
            try:
                sock = socket.create_connection((args.host, args.port))
            except OSError as err:
                sys.exit(f"cannot connect to {args.host}:{args.port}: {err}")
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f"[run_native] connected to {args.host}:{args.port}", flush=True)
            pump(master, sock)
            sock.close()
            print("[run_native] connection closed", flush=True)
            return 0
    except KeyboardInterrupt:
        return 0
    finally:
        os.unlink(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="start zephyr.exe and expose its DYNAMIXEL bus")
    run.add_argument("--exe", default=DEFAULT_EXE, help="native_sim program (%(default)s)")
    run.add_argument("--link", default=os.path.join(ROOT, "build-native", "ttyDXL"),
                     help="fixed path of the bus PTY (%(default)s)")
    run.add_argument("--tcp", type=int, metavar="PORT", help="also serve the bus on this TCP port")
    run.add_argument("zephyr_args", nargs="*", help="options of zephyr.exe, after --")

    con = sub.add_parser("connect", help="local PTY connected to the TCP bridge of \"run\"")
    con.add_argument("--host", default="localhost")
    con.add_argument("--port", type=int, default=5555)
    con.add_argument("--link", default="/tmp/ttyDXL", help="path of the local PTY (%(default)s)")

    args = parser.parse_args()
    return cmd_run(args) if args.command == "run" else cmd_connect(args)


if __name__ == "__main__":
    sys.exit(main())
