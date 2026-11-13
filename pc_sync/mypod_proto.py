"""MyPod USB sync protocol (PC side). See src/usbsync_engine.h for the device side.

Frame:  A5 5A | cmd | len (u16 LE) | payload | crc16 (u16 LE, CRC-16/CCITT-FALSE over cmd,len,payload)
Replies: cmd|0x80, or 0xFF = error [orig cmd][code][text]. Uploads are stop-and-wait.
"""
import struct
import time
import zlib

MAGIC = b"\xA5\x5A"
CMD_HELLO, CMD_SETBAUD, CMD_PING, CMD_FREE = 0x01, 0x02, 0x03, 0x04
CMD_PUT_BEGIN, CMD_PUT_DATA, CMD_PUT_END, CMD_PUT_ABORT = 0x10, 0x11, 0x12, 0x13
CMD_LIST, CMD_DELETE = 0x20, 0x21
CMD_EXIT = 0x30
R_LISTENT, R_LISTDONE, R_ERROR = 0xA0, 0xA1, 0xFF

CHUNK = 4096
MAX_PAYLOAD = 4200
PROTO_VER = 1


class DeviceError(Exception):
    def __init__(self, code, text):
        super().__init__(f"{text} (code {code})")
        self.code, self.text = code, text


class TimeoutError_(Exception):
    pass


class Cancelled(Exception):
    pass


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(cmd, payload=b""):
    body = bytes([cmd]) + struct.pack("<H", len(payload)) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


class FrameReader:
    """Incremental parser; skips boot-log garbage and corrupt frames."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                if len(self.buf) > 1:
                    del self.buf[:-1]            # keep a possible lone 0xA5
                break
            if i:
                del self.buf[:i]
            if len(self.buf) < 5:
                break
            ln = self.buf[3] | (self.buf[4] << 8)
            if ln > MAX_PAYLOAD:
                del self.buf[:2]
                continue
            total = 2 + 3 + ln + 2
            if len(self.buf) < total:
                break
            body = bytes(self.buf[2:5 + ln])
            crc = self.buf[5 + ln] | (self.buf[6 + ln] << 8)
            if crc16(body) == crc:
                out.append((body[0], bytes(body[3:])))
                del self.buf[:total]
            else:
                del self.buf[:2]                  # false magic / corruption: resync
        return out


class SerialTransport:
    def __init__(self, port, baud=115200):
        import serial
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.05
        self.ser.write_timeout = 5
        self.ser.dtr = False                     # don't pulse EN: opening the port must not reboot the pod
        self.ser.rts = False
        self.ser.open()

    def write(self, data):
        self.ser.write(data)
        self.ser.flush()

    def read(self, timeout):
        self.ser.timeout = timeout
        data = self.ser.read(1)
        if data:
            n = self.ser.in_waiting
            if n:
                data += self.ser.read(n)
        return data

    def set_baud(self, baud):
        self.ser.baudrate = baud
        time.sleep(0.05)
        self.ser.reset_input_buffer()

    def flush_input(self):
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


class Device:
    def __init__(self, transport, log=None):
        self.t = transport
        self.fr = FrameReader()
        self.log = log or (lambda *_: None)
        self.pending = []                        # frames received while waiting for something else

    # ------------------------------------------------------------------ low level
    def _send(self, cmd, payload=b""):
        self.t.write(build_frame(cmd, payload))

    def _next_frame(self, timeout):
        if self.pending:
            return self.pending.pop(0)
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError_("no answer from the iPod")
            data = self.t.read(min(left, 0.2))
            if data:
                frames = self.fr.feed(data)
                if frames:
                    self.pending.extend(frames[1:])
                    return frames[0]

    def request(self, cmd, payload=b"", timeout=3.0, retries=2):
        last = None
        for attempt in range(retries + 1):
            self._send(cmd, payload)
            try:
                while True:
                    rcmd, rpl = self._next_frame(timeout)
                    if rcmd == (cmd | 0x80):
                        return rpl
                    if rcmd == R_ERROR and len(rpl) >= 2 and rpl[0] == cmd:
                        raise DeviceError(rpl[1], rpl[2:].decode("ascii", "replace"))
                    # stale frame from an earlier retry: ignore
            except TimeoutError_ as e:
                last = e
                self.log(f"retry {attempt + 1} for command 0x{cmd:02x}")
        raise last

    # ------------------------------------------------------------------ commands
    def hello(self, tries=8):
        """Wait for the pod (it may still be booting after the port opened)."""
        last = None
        for _ in range(tries):
            try:
                self._send(CMD_PING)
                self._send(CMD_HELLO)
                end = time.time() + 1.2
                while time.time() < end:
                    try:
                        rcmd, rpl = self._next_frame(end - time.time())
                    except TimeoutError_:
                        break
                    if rcmd == (CMD_HELLO | 0x80) and len(rpl) >= 7:
                        n = rpl[6]
                        return {"proto": rpl[0], "sd": bool(rpl[1]),
                                "card_mb": struct.unpack("<I", rpl[2:6])[0],
                                "name": rpl[7:7 + n].decode("ascii", "replace")}
            except Exception as e:                # noqa
                last = e
        raise TimeoutError_("The iPod did not answer. Is it in USB sync mode? "
                            "(Settings > Copy songs from PC) " + (str(last) if last else ""))

    def free_space(self):
        rpl = self.request(CMD_FREE, timeout=60, retries=0)
        total, free = struct.unpack("<II", rpl)
        return total * 1024, free * 1024

    def set_baud(self, baud):
        """Ask for a faster link; falls back to the current speed on any problem."""
        old = getattr(self.t, "ser", None) and self.t.ser.baudrate
        try:
            self.request(CMD_SETBAUD, struct.pack("<I", baud), timeout=1.5, retries=1)
        except Exception as e:
            self.log(f"speed change refused ({e})")
            return False
        self.t.set_baud(baud)
        self.fr = FrameReader()
        self.pending = []
        for _ in range(3):
            try:
                self.request(CMD_PING, timeout=0.8, retries=0)
                return True
            except Exception:
                pass
        self.log("fast link not working, going back to the slow speed")
        if old:
            self.t.set_baud(old)
            time.sleep(5.5)                       # the pod falls back by itself after 5 s
            self.fr = FrameReader()
            self.pending = []
        return False

    def put(self, dev_path, source, progress=None, cancel=None):
        """Upload bytes or a local file path to dev_path on the SD card."""
        if isinstance(source, (bytes, bytearray)):
            size = len(source)
            fh = None
        else:
            import os
            size = os.path.getsize(source)
            fh = open(source, "rb")
        try:
            p = dev_path.encode("utf-8")
            self.request(CMD_PUT_BEGIN, struct.pack("<IB", size, len(p)) + p, timeout=10)
            crc = 0
            off = 0
            while off < size:
                if cancel and cancel():
                    try:
                        self.request(CMD_PUT_ABORT, timeout=3, retries=0)
                    except Exception:
                        pass
                    raise Cancelled()
                chunk = bytes(source[off:off + CHUNK]) if fh is None else fh.read(CHUNK)
                if not chunk:
                    raise IOError("file shrank while sending")
                self.request(CMD_PUT_DATA, struct.pack("<I", off) + chunk, timeout=6, retries=3)
                crc = zlib.crc32(chunk, crc)
                off += len(chunk)
                if progress:
                    progress(off, size)
            self.request(CMD_PUT_END, struct.pack("<I", crc & 0xFFFFFFFF), timeout=15)
        finally:
            if fh:
                fh.close()

    def list_songs(self):
        self._send(CMD_LIST)
        songs = []
        while True:
            rcmd, rpl = self._next_frame(15)
            if rcmd == R_LISTENT and len(rpl) > 5:
                size, has = struct.unpack("<IB", rpl[:5])
                songs.append({"path": rpl[5:].decode("utf-8", "replace"), "size": size, "cover": bool(has)})
            elif rcmd == R_LISTDONE:
                return songs
            elif rcmd == R_ERROR:
                raise DeviceError(rpl[1] if len(rpl) > 1 else 0, rpl[2:].decode("ascii", "replace"))

    def delete(self, dev_path):
        self.request(CMD_DELETE, dev_path.encode("utf-8"), timeout=10)

    def exit_to_player(self):
        try:
            self.request(CMD_EXIT, timeout=3, retries=0)
        except TimeoutError_:
            pass
