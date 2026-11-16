"""Receive-only capture of the wallpad RS485 bus through an EW11 (RS485 -> TCP).

  py -3 tools/rs485_log.py --host <EW11-IP>                 # 10 min capture, live "new packet" view
  py -3 tools/rs485_log.py --host <EW11-IP>  --seconds 300
  py -3 tools/rs485_log.py --analyze tools/rs485-logs/xxx.log # summarize an existing capture

This tool never sends anything to the EW11 or the bus: it only opens a TCP
connection and reads. Packets are split with the Samsung SDS rule observed in
public analyses: a packet starts with a byte >= 0x80, and its last byte is the
XOR of the preceding bytes with bit 7 cleared.
EW11 serial settings must match the bus (SDS: 9600 baud, 8 data bits, even parity, 1 stop bit).
"""
import argparse
import collections
import datetime
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LENGTHS = (4, 5, 8, 6, 7, 9, 10, 11, 12, 23)

# Meaning of headers / commands as described in public SDS wallpad analyses (unverified for this home).
HEADERS = {0xAC: 'light', 0xAD: 'entrance switch', 0xAE: 'heating', 0xAB: 'gas', 0xAA: 'energy meter',
           0xC2: 'ventilation', 0xC6: 'outlet', 0xCC: 'entrance switch (new)', 0xCE: 'unknown (CE)',
           0xC1: 'unknown (C1)', 0xC3: 'unknown (C3)', 0xB0: 'device reply',
           **{h: f'intercom {h - 0xA0}' for h in range(0xA1, 0xA7)}}
COMMANDS = {0x41: 'poll/keepalive', 0x5A: 'scan', 0x7C: 'heating status', 0x7D: 'heating on/off',
            0x7F: 'heating set temp', 0x2F: 'elevator call', 0x10: 'elevator call (new)', 0x12: 'elevator arrived',
            0x56: 'gas status', 0x78: 'gas close', 0x79: 'light status', 0x7A: 'light command', 0x4A: 'outlet status',
            0x6E: 'outlet power', 0x4B: 'outlet standby cut', 0x4E: 'ventilation status', 0x4F: 'ventilation command',
            0x6F: 'meter reading', 0x31: 'doorbell home door', 0x32: 'doorbell common door', 0x3E: 'bell off',
            0x42: 'intercom busy', 0x35: 'video connected', 0x36: 'call accepted', 0x38: 'video call',
            0x3B: 'door open', 0x01: 'clock push', 0x08: 'away mode', 0x0B: 'device states', 0x13: 'gas close (switch)',
            0x52: 'batch light query', 0x54: 'batch light off', 0x55: 'gas close (switch)'}


def checksum_ok(packet):
    if len(packet) < 3:
        return False
    value = 0
    for byte in packet[:-1]:
        value ^= byte
    return (value & 0x7F) == packet[-1]


def split_packets(data):
    """Split a byte stream into SDS packets. Unparseable bytes are returned as junk packets."""
    packets, i, junk = [], 0, bytearray()
    while i < len(data):
        found = None
        if data[i] >= 0x80:
            for length in LENGTHS:
                candidate = data[i:i + length]
                if len(candidate) < length:
                    break
                follows = i + length >= len(data) or data[i + length] >= 0x80
                if follows and checksum_ok(candidate):
                    found = bytes(candidate)
                    break
        if found:
            if junk:
                packets.append(('junk', bytes(junk)))
                junk = bytearray()
            packets.append(('ok', found))
            i += len(found)
        else:
            junk.append(data[i])
            i += 1
    if junk:
        packets.append(('junk', bytes(junk)))
    return packets


def pattern(packet):
    """Group key: header + command (values in the payload may vary)."""
    return packet[:2]


def describe(packet):
    head = HEADERS.get(packet[0], '?')
    command = COMMANDS.get(packet[1], '') if len(packet) > 1 else ''
    return f'{head}{" / " + command if command else ""}'


def hexs(data):
    return ' '.join(f'{b:02X}' for b in data)


def summarize(packets, out=sys.stdout):
    good = [p for kind, p in packets if kind == 'ok']
    junk = sum(len(p) for kind, p in packets if kind == 'junk')
    total = sum(len(p) for _, p in packets) or 1
    print(f'\nPackets: {len(good)}  checksum-valid bytes: {100 * (total - junk) / total:.1f}%  '
          f'unparsed bytes: {junk}', file=out)
    if junk / total > 0.3:
        print('  Many unparsed bytes: check the EW11 serial settings (9600 8E1) or the protocol.', file=out)
    by_pattern = collections.Counter(pattern(p) for p in good)
    print('\nBy header/command:', file=out)
    for key, count in by_pattern.most_common():
        print(f'  {hexs(key):6}  {count:6}  {describe(key)}', file=out)
    distinct = collections.Counter(good)
    print('\nDistinct packets (most frequent first, max 40):', file=out)
    for packet, count in distinct.most_common(40):
        print(f'  {count:6}  {hexs(packet):30}  {describe(packet)}', file=out)


def start_markers(log, lock):
    """Each Enter (optionally with a note) writes a timestamped MARK line into the log."""
    import threading

    def run():
        count = 0
        for line in sys.stdin:
            count += 1
            now = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
            note = line.strip() or f'step {count}'
            with lock:
                log.write(f'{now} MARK {note}\n')
                log.flush()
            print(f'{now}  ===== MARK {count}: {note} =====', flush=True)
    threading.Thread(target=run, daemon=True).start()


def capture(host, port, seconds, path, markers=False):
    import threading
    path.parent.mkdir(parents=True, exist_ok=True)
    seen, stream = set(), bytearray()
    lock = threading.Lock()
    deadline = time.monotonic() + seconds
    print(f'Connecting to {host}:{port} (receive only). Writing {path}')
    with socket.create_connection((host, port), timeout=5) as sock, path.open('w', encoding='utf-8') as log:
        sock.settimeout(1)
        print(f'Capturing for {seconds} s. Use the wallpad / thermostat now; new packet types are shown live.')
        if markers:
            start_markers(log, lock)
            print('Press Enter right when you do each action (you may type a note first, e.g. "on").')
        print('Press Ctrl+C to stop early.\n')
        try:
            while time.monotonic() < deadline:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    print('EW11 closed the connection.')
                    break
                now = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
                with lock:
                    log.write(f'{now} {chunk.hex()}\n')
                    log.flush()
                stream.extend(chunk)
                # Keep the tail that may be an incomplete packet.
                parsed = split_packets(bytes(stream[:-12])) if len(stream) > 24 else []
                consumed = sum(len(p) for _, p in parsed)
                stream = stream[consumed:]
                for kind, packet in parsed:
                    key = packet if kind == 'ok' and packet[1:2] != b'\x41' else pattern(packet)
                    if kind == 'ok' and key not in seen:
                        seen.add(key)
                        print(f'{now}  NEW  {hexs(packet):30}  {describe(packet)}')
        except KeyboardInterrupt:
            print('\nStopped.')
    return path


def read_log(path):
    data = bytearray()
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] != 'MARK':
            try:
                data.extend(bytes.fromhex(parts[1]))
            except ValueError:
                pass
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', help='EW11 IP address')
    parser.add_argument('--port', type=int, default=8899, help='EW11 TCP server port (EW11 default 8899)')
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mark', action='store_true', help='press Enter during capture to insert action markers')
    parser.add_argument('--analyze', type=Path, help='summarize an existing capture instead of capturing')
    args = parser.parse_args()
    if args.analyze:
        path = args.analyze
    elif args.host:
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        path = capture(args.host, args.port, args.seconds, args.out or ROOT/'tools'/'rs485-logs'/f'capture-{stamp}.log', args.mark)
    else:
        parser.error('give --host (capture) or --analyze (existing log)')
    summarize(split_packets(read_log(path)))


if __name__ == '__main__':
    main()
