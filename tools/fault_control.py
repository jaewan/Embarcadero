#!/usr/bin/env python3
"""Owned local control endpoint for compile-enabled, deterministic fault tests.

This is a SOCK_SEQPACKET control channel, never a network listener. Keep the
child endpoint open through process creation, pass its FD explicitly, then
close the parent's copy. Record every command/event with a monotonic timestamp.
A missing HIT is a failed schedule, never a successful fault test.
"""
import json
import os
from pathlib import Path
import secrets
import socket
import time


class FaultControl:
    def __init__(self, artifact, token=None):
        self.parent, self.child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.token = token or secrets.token_hex(16)
        self.artifact = Path(artifact)
        self.events = []
        self.pending = []
        self.next_id = 1

    def environment(self):
        return {"EMBARCADERO_FAULT_CONTROL_FD": str(self.child.fileno()),
                "EMBARCADERO_FAULT_CONTROL_TOKEN": self.token}

    @property
    def pass_fds(self):
        return (self.child.fileno(),)

    def child_started(self):
        self.child.close()

    def _record(self, direction, packet):
        self.events.append({"monotonic_ns": time.monotonic_ns(), "direction": direction, "packet": packet})
        self.artifact.parent.mkdir(parents=True, exist_ok=True)
        self.artifact.write_text(json.dumps(self.events, indent=2) + "\n")

    def send(self, packet):
        data = packet.encode("ascii")
        if not data or len(data) > 512:
            raise ValueError("invalid fault control packet size")
        self.parent.settimeout(1.0)
        if self.parent.send(data) != len(data):
            raise RuntimeError("partial fault-control command")
        self._record("send", packet)

    def wait(self, prefix, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            for index, packet in enumerate(self.pending):
                if packet == prefix or packet.startswith(prefix + " "):
                    return self.pending.pop(index)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("fault schedule did not reach " + prefix)
            self.parent.settimeout(remaining)
            packet = self.parent.recv(513)
            if not packet or len(packet) > 512:
                raise RuntimeError("fault controller disconnected or sent an oversized event")
            text = packet.decode("ascii")
            self._record("receive", text)
            if text.startswith("ERROR"):
                raise RuntimeError(text)
            self.pending.append(text)

    def ready(self, timeout=120):
        event = self.wait("READY", timeout)
        if event != "READY " + self.token:
            raise RuntimeError("fault controller run-token mismatch")

    def arm(self, name, client="*", epoch="*", batch="*", value=0):
        # Before START this is ordered before the first hook. After START the
        # caller must hold an earlier reached barrier or otherwise prove the
        # selected transition has not occurred; absence of HIT is a failure.
        identity = self.next_id
        self.next_id += 1
        self.send(f"ARM {identity} {name} {client} {epoch} {batch} {value}")
        self.wait(f"ARMED {identity}")
        return identity

    def start(self):
        self.send("START")
        self.wait("STARTED")

    def hit(self, identity, timeout=10):
        packet = self.wait(f"HIT {identity}", timeout)
        fields = packet.split()
        if len(fields) != 9:
            raise RuntimeError("malformed HIT event")
        if int(fields[8]) <= 0:
            raise RuntimeError("invalid HIT native thread identity")
        return {"id": int(fields[1]), "name": fields[2],
                **dict(zip(("client", "epoch", "batch", "detail0", "detail1", "tid"),
                           map(int, fields[3:])))}

    def release(self, identity):
        self.send(f"RELEASE {identity}")
        self.wait(f"RELEASED {identity}")

    def close(self):
        try:
            self.send("CANCEL")
        except OSError:
            pass
        self.parent.close()
        self.child.close()
