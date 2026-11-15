"""Debug: capture the panel screen over USB serial (and optionally tap first).

  py -3 tools/screenshot.py out.png                 # capture current screen
  py -3 tools/screenshot.py out.png --tap 100 180   # tap (x, y), wait, capture
Close the serial monitor and the bridge first. Opening the port does not reset the board.
"""
import argparse
import time
import serial
from PIL import Image


def open_port(name):
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = name, 115200, 5
    port.dtr = False
    port.rts = False
    port.open()
    return port


def capture(port):
    port.reset_input_buffer()
    port.write(b'SHOT\n')
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        line = port.readline().decode('ascii', 'ignore').strip()
        if line.startswith('SHOT|'):
            _, w, h = line.split('|')
            w, h = int(w), int(h)
            # The panel may be slow to send while networking is busy: wait up to 60 s.
            data, total, until = bytearray(), w * h * 2, time.monotonic() + 60
            while len(data) < total and time.monotonic() < until:
                data += port.read(total - len(data))
            if len(data) != total:
                raise RuntimeError(f'short read: {len(data)} bytes')
            image = Image.new('RGB', (w, h))
            pixels = []
            for i in range(0, len(data), 2):
                v = data[i] | (data[i + 1] << 8)
                r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
                pixels.append((r * 255 // 31, g * 255 // 63, b * 255 // 31))
            image.putdata(pixels)
            return image
    raise RuntimeError('no SHOT header received')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output')
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--tap', nargs=2, type=int, action='append', default=[])
    parser.add_argument('--wait', type=float, default=0.6)
    args = parser.parse_args()
    port = open_port(args.port)
    try:
        for x, y in args.tap:
            port.write(f'TAP|{x}|{y}\n'.encode('ascii'))
            time.sleep(args.wait)
        capture(port).save(args.output)
        print('saved', args.output)
    finally:
        port.close()


if __name__ == '__main__':
    main()
