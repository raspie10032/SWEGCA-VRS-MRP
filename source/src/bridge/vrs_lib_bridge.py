#!/usr/bin/env python3
"""Claude Code stdio MCP to one local libvrs daemon. Python standard library only."""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import datetime
import getpass
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time


class BridgeError(Exception):
    pass


def linux_process(pid: int) -> tuple[int, str, int]:
    with open(f"/proc/{pid}/stat", encoding="utf-8") as stream:
        fields = stream.read().rsplit(")", 1)[1].split()
    parent, ticks = int(fields[1]), fields[19]
    with open("/proc/stat", encoding="utf-8") as stream:
        boot = next(int(line.split()[1]) for line in stream if line.startswith("btime "))
    epoch = boot + int(ticks) // os.sysconf("SC_CLK_TCK")
    return parent, ticks, epoch


def windows_process(pid: int) -> tuple[int, str, int]:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]

    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise BridgeError("PROCESS_SNAPSHOT_FAILED")
    parent = None
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            if entry.th32ProcessID == pid:
                parent = int(entry.th32ParentProcessID)
                break
            ok = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    if parent is None:
        raise BridgeError("PROCESS_NOT_ALIVE")
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        raise BridgeError("PROCESS_START_UNREADABLE")
    try:
        creation, exit_time, kernel_time, user_time = (wintypes.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time),
                                      ctypes.byref(kernel_time), ctypes.byref(user_time)):
            raise BridgeError("PROCESS_START_UNREADABLE")
        ticks = (creation.dwHighDateTime << 32) | creation.dwLowDateTime
        return parent, str(ticks), ticks // 10_000_000 - 11_644_473_600
    finally:
        kernel.CloseHandle(handle)


def process(pid: int) -> tuple[int, str, int]:
    return windows_process(pid) if os.name == "nt" else linux_process(pid)


def start_matches(recorded: object, native: str, epoch: int) -> bool:
    if not isinstance(recorded, (str, int)) or isinstance(recorded, bool):
        return False
    text = str(recorded)
    if text == native:
        return True
    # Windows releases have recorded .NET DateTime ticks as well as FILETIME.
    # Both compare the same exact 100 ns creation time; neither uses a tolerance.
    if os.name == "nt" and text == str(int(native) + 504_911_232_000_000_000):
        return True
    dt = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    # The UTC asctime variant has exactly one-second precision by definition.
    days = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
    months = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
    asctime = f"{days[dt.weekday()]} {months[dt.month - 1]} {dt.day:2d} {dt:%H:%M:%S %Y}"
    return text == asctime


class Identity:
    def __init__(self, projects: Path):
        self.projects = projects.resolve()
        self.cache: dict[str, Path] = {}

    def resolve(self) -> str:
        test_file = os.environ.get("VRS_CC_TEST_SESSION_FILE")
        if test_file:
            candidate = Path(test_file)
            with candidate.open(encoding="utf-8") as stream:
                record = json.load(stream)
            pid = record.get("pid")
            if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
                raise BridgeError("SESSION_PID_INVALID")
        else:
            sessions = Path.home() / ".claude" / "sessions"
            pid = os.getpid()
            record = None
            for _ in range(4):
                pid = process(pid)[0]
                if pid <= 0:
                    break
                candidate = sessions / f"{pid}.json"
                if candidate.is_file():
                    with candidate.open(encoding="utf-8") as stream:
                        record = json.load(stream)
                    break
            if record is None:
                raise BridgeError("CALLER_SESSION_NOT_FOUND_IN_4_PARENTS")
        if record.get("pid") != pid:
            raise BridgeError("SESSION_PID_MISMATCH")
        _, native, epoch = process(pid)
        if not start_matches(record.get("procStart"), native, epoch):
            raise BridgeError("SESSION_PROC_START_MISMATCH")
        sid = record.get("sessionId")
        if not isinstance(sid, str) or not re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", sid):
            raise BridgeError("SESSION_ID_INVALID")
        if sid not in self.cache:
            paths = list(self.projects.glob(f"*/{sid}.jsonl"))
            if len(paths) != 1:
                raise BridgeError("SESSION_TRANSCRIPT_NOT_UNIQUE")
            resolved = paths[0].resolve(strict=True)
            if not resolved.is_relative_to(self.projects):
                raise BridgeError("SESSION_TRANSCRIPT_OUTSIDE_ROOT")
            self.cache[sid] = resolved
        path = self.cache[sid].resolve(strict=True)
        if not path.is_relative_to(self.projects):
            raise BridgeError("SESSION_TRANSCRIPT_OUTSIDE_ROOT")
        return str(path)


class DaemonClient:
    def __init__(self):
        self.projects = Path(os.environ.get("VRS_CC_PROJECTS", str(Path.home() / ".claude" / "projects")))
        self.data = Path(os.environ.get("VRS_CC_DATA", str(Path.home() / ".claude" / "vrs-lib")))
        default = rf"\\.\pipe\vrs-lib-{getpass.getuser()}" if os.name == "nt" else f"@vrs-lib-{os.getuid()}"
        self.endpoint = os.environ.get("VRS_CC_ENDPOINT", default)
        self.identity = Identity(self.projects)
        self.spawned: subprocess.Popen | None = None

    def spawn(self):
        default = Path(__file__).resolve().parents[1] / "build" / ("vrs_ccd.exe" if os.name == "nt" else "vrs_ccd")
        executable = Path(os.environ.get("VRS_CC_DAEMON", str(default)))
        if not executable.is_file():
            raise BridgeError("DAEMON_BINARY_NOT_FOUND")
        args = [str(executable), "--projects", str(self.projects), "--data", str(self.data), "--endpoint", self.endpoint]
        for root in os.environ.get("VRS_CC_READ_ROOTS", "").split(os.pathsep):
            if root:
                args.extend(["--read-root", root])
        kwargs = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL, "close_fds": True}
        if os.name == "nt":
            kwargs["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            kwargs["start_new_session"] = True
        self.spawned = subprocess.Popen(args, **kwargs)

    def start_early(self):
        # D-KM6a (doc 14.16): on MCP initialize, probe the daemon once and spawn it if absent, then return at once.
        # The first index builds while the session runs; nothing here waits for it.
        try:
            self.connect().close()
        except OSError:
            try:
                self.spawn()
            except (BridgeError, OSError):
                pass  # a tool call reports the same failure by name

    def connect(self):
        if os.name == "nt":
            # Binary unbuffered IO preserves the daemon's one-line UTF-8 frames.
            return open(self.endpoint, "r+b", buffering=0)
        if not self.endpoint.startswith("@"):
            raise BridgeError("SOCKET_EXPECT_ABSTRACT_NAME")
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(5)
        try:
            sock.connect("\0" + self.endpoint[1:])
        except BaseException:
            sock.close()
            raise
        return sock

    def call(self, request: dict) -> dict:
        deadline = time.monotonic() + 5
        spawned = False
        while True:
            try:
                connection = self.connect()
                break
            except OSError as exc:
                if not spawned:
                    self.spawn()
                    spawned = True
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise BridgeError("DAEMON_CONNECT_TIMEOUT_5S") from exc
                time.sleep(min(0.05, remaining))
        data = (json.dumps(request, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        try:
            if isinstance(connection, socket.socket):
                connection.sendall(data)
                read = connection.recv
            else:
                at = 0
                while at < len(data):
                    n = connection.write(data[at:])
                    if not n:
                        raise BridgeError("DAEMON_WRITE_FAILED")
                    at += n
                read = connection.read
            chunks = bytearray()
            while len(chunks) <= 8 * 1024 * 1024:
                part = read(65536)
                if not part:
                    raise BridgeError("DAEMON_INCOMPLETE_REPLY")
                chunks.extend(part)
                if b"\n" in part:
                    reply = json.loads(bytes(chunks).split(b"\n", 1)[0].decode("utf-8"))
                    if not reply.get("ok"):
                        raise BridgeError(reply.get("error", "DAEMON_ERROR"))
                    return reply
            raise BridgeError("DAEMON_REPLY_TOO_LARGE")
        finally:
            connection.close()


def tools_list() -> dict:
    budget = {"type": "integer", "minimum": 1, "maximum": 95000, "default": 48000,
              "description": "Maximum UTF-16 units of the full rendered result."}
    return {"tools": [
        {"name": "vrs_restore", "description": "Restore selected original user utterances and assistant answers before this session's last compaction. Pass next as start for another page.",
         "inputSchema": {"type": "object", "properties": {"budget": budget, "start": {"type": "string", "pattern": "^[0-9]+:[0-9]+(?::[0-9]+)?$", "description": "Original o:k:c position; c is a UTF-16 body offset. Legacy o:k means c=0."}}, "additionalProperties": False},
         "_meta": {"anthropic/maxResultSizeChars": 100000}},
        {"name": "vrs_read", "description": "Read an exact UTF-8 byte range under the allowed roots, with SHA-256. Pages end at line boundaries.",
         "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "start": {"type": "integer", "minimum": 0},
                         "end": {"type": "integer", "minimum": 0}, "budget": budget}, "required": ["path", "start", "end"], "additionalProperties": False},
         "_meta": {"anthropic/maxResultSizeChars": 100000}},
    ]}


# D-KM6b (doc 14.16): NOT_READY means the index is still being prepared, not that the originals are missing.
NOT_READY_TEXT = ("NOT_READY: the vrs-lib index is still being prepared (its daemon has just started). "
                  "This does not mean the originals are missing. Call the same tool again with the same arguments in a few seconds.")


def invoke(client: DaemonClient, params: dict) -> dict:
    try:
        name, args = params.get("name"), params.get("arguments", {})
        if not isinstance(args, dict):
            raise BridgeError("ARGUMENTS_EXPECT_OBJECT")
        if name == "vrs_restore":
            if set(args) - {"budget", "start"}:
                raise BridgeError("UNKNOWN_ARGUMENT")
            request = dict(args, op="restore", transcript=client.identity.resolve())
        elif name == "vrs_read":
            if set(args) - {"path", "start", "end", "budget"} or not {"path", "start", "end"} <= set(args):
                raise BridgeError("READ_ARGUMENTS_INVALID")
            request = dict(args, op="read")
        else:
            raise BridgeError("UNKNOWN_TOOL")
        reply = client.call(request)
        return {"content": [{"type": "text", "text": reply["text"]}], "isError": False}
    except (BridgeError, OSError, ValueError, KeyError, TypeError) as exc:
        text = NOT_READY_TEXT if str(exc) == "NOT_READY" else str(exc)[:2000]
        return {"content": [{"type": "text", "text": text}], "isError": True}


def main():
    client = DaemonClient()
    for line in sys.stdin.buffer:
        request = None
        try:
            request = json.loads(line.decode("utf-8"))
            if not isinstance(request, dict):
                raise ValueError("JSONRPC_EXPECT_OBJECT")
            if "id" not in request:
                continue
            method = request.get("method")
            if method == "initialize":
                version = request.get("params", {}).get("protocolVersion", "2025-03-26")
                result = {"protocolVersion": version, "capabilities": {"tools": {}}, "serverInfo": {"name": "vrs-lib", "version": "1.0.0"}}
                client.start_early()
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = tools_list()
            elif method == "tools/call":
                result = invoke(client, request.get("params", {}))
            else:
                raise BridgeError("METHOD_NOT_FOUND")
            response = {"jsonrpc": "2.0", "id": request["id"], "result": result}
        except Exception as exc:
            response = {"jsonrpc": "2.0", "id": request.get("id") if isinstance(request, dict) else None,
                        "error": {"code": -32601 if isinstance(exc, BridgeError) else -32700, "message": str(exc)[:2000]}}
        sys.stdout.buffer.write((json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
