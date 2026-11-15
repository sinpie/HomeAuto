"""Optional USB gateway between the wall panel and a Home Assistant server.

Set HOME_ASSISTANT_TOKEN in the environment, fill entities in your private
bridge.json, then: python tools/ha_bridge.py --config bridge.json

Panel features and the entities they may use (anything else is rejected):
  office_call  script.* or button.*           management office call
  elevator     floor: sensor.*, call: script.* or button.*, home_floor: int
  doors        3 x binary_sensor.* / event.*  one-way calls: 현관, 1층 현관, 지하주차장 문

Serial protocol (one line each, ASCII):
  panel -> PC   CMD|E|call   CMD|O|call
  PC -> panel   PING
                ELEV|<floor>|<dir -1/0/1>|<calling 0/1>   (floor 0 = unknown, B1 = -1)
                ELEVERR
                OFFICE|<calling 0/1>   OFFICEERR
                RING|<door 0..2>
Command success is only reported after the server accepted the service call.
The boiler is not handled here: the panel reads it directly from the wallpad bus.
"""
import argparse
import json
import os
import queue
import re
import threading
import time
import urllib.request
from pathlib import Path

UNKNOWN = -9999
CALL_DOMAINS = r'(script|button)'
OFFICE_CALL_SECONDS = 30
ELEVATOR_CALL_TIMEOUT = 180


def parse_command(line):
    if line in ('CMD|E|call', 'CMD|O|call'):
        return (line[4], 'call')
    return None


def _entity(value, pattern, name):
    if value is None:
        return None
    if not isinstance(value, str) or not re.fullmatch(pattern + r'\.[a-z0-9_]+', value):
        raise ValueError(f'{name} has an unsupported entity: {value!r}')
    return value


def validate_config(config):
    from urllib.parse import urlsplit
    url = urlsplit(config['base_url'])
    if url.scheme not in ('http', 'https') or not url.hostname or url.username or url.password:
        raise ValueError('base_url must be an HTTP(S) server URL without credentials')
    if url.query or url.fragment or url.path.rstrip('/'):
        raise ValueError('base_url must be the server root URL')
    _entity(config.get('office_call'), CALL_DOMAINS, 'office_call')
    elevator = config.get('elevator')
    if elevator is not None:
        _entity(elevator.get('floor'), 'sensor', 'elevator.floor')
        _entity(elevator.get('call'), CALL_DOMAINS, 'elevator.call')
        if not elevator.get('floor') or not elevator.get('call'):
            raise ValueError('elevator needs both floor and call entities')
        home = elevator.get('home_floor')
        if not isinstance(home, int) or home == 0 or not -9 <= home <= 199:
            raise ValueError('elevator.home_floor must be a floor number (B1 = -1)')
    doors = config.get('doors', [None, None, None])
    if len(doors) != 3:
        raise ValueError('doors must have three entries: 현관, 1층 현관, 지하주차장 문')
    for door in doors:
        _entity(door, r'(binary_sensor|event)', 'doors')


def parse_floor(state):
    if not isinstance(state, str):
        return 0
    state = state.strip().upper()
    match = re.fullmatch(r'B(\d)', state)
    if match:
        return -int(match[1]) if match[1] != '0' else 0
    if re.fullmatch(r'-?\d{1,3}', state):
        floor = int(state)
        return floor if floor != 0 and -9 <= floor <= 199 else 0
    return 0


def to_x10(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return UNKNOWN
    return round(number * 10) if -40 <= number <= 90 else UNKNOWN


def door_rang(entity, previous, current):
    """True when a door entity reports a new call."""
    if previous is None or current in (None, 'unavailable', 'unknown'):
        return False
    if entity.startswith('binary_sensor.'):
        return previous != 'on' and current == 'on'
    return current != previous  # event.* state is the time of the last event


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        # Do not forward authorization to a redirect target.
        return None


class Gateway:
    def __init__(self, config, token, clock=time.monotonic):
        validate_config(config)
        self.base = config['base_url'].rstrip('/')
        self.office = config.get('office_call')
        self.elevator = config.get('elevator')
        self.doors = config.get('doors', [None, None, None])
        self.token = token
        self.clock = clock
        self.outgoing = queue.Queue(maxsize=128)
        self.commands = queue.Queue(maxsize=16)
        self.stop = threading.Event()
        self.opener = urllib.request.build_opener(NoRedirect)
        self.floor = 0
        self.elevator_called_at = None
        self.office_until = 0
        self.door_states = [None, None, None]

    def request(self, path, payload=None):
        data = None if payload is None else json.dumps(payload).encode('utf-8')
        req = urllib.request.Request(self.base + '/api/' + path, data=data,
            headers={'Authorization': 'Bearer ' + self.token, 'Content-Type': 'application/json'})
        with self.opener.open(req, timeout=3) as response:
            return json.load(response)

    def state(self, entity):
        try:
            return self.request('states/' + entity)
        except Exception:
            return {}

    def call(self, entity, service=None, extra=None):
        domain = entity.split('.')[0]
        service = service or ('press' if domain == 'button' else 'turn_on')
        self.request(f'services/{domain}/{service}', dict({'entity_id': entity}, **(extra or {})))

    def emit(self, line):
        try:
            self.outgoing.put_nowait(line)
        except queue.Full:
            pass

    # ----- reports -----
    def report_elevator(self):
        if not self.elevator:
            self.emit('ELEV|0|0|0')
            return
        floor = parse_floor(self.state(self.elevator['floor']).get('state'))
        direction = 0
        if floor and self.floor and floor != self.floor:
            direction = 1 if floor > self.floor else -1
        if floor:
            self.floor = floor
        calling = self.elevator_called_at is not None
        if calling and (floor == self.elevator['home_floor']
                        or self.clock() - self.elevator_called_at > ELEVATOR_CALL_TIMEOUT):
            self.elevator_called_at = None
            calling = False
        self.emit(f'ELEV|{floor}|{direction if floor else 0}|{int(calling)}')

    def report_office(self):
        self.emit(f'OFFICE|{int(self.clock() < self.office_until)}')

    def check_doors(self):
        for index, entity in enumerate(self.doors):
            if not entity:
                continue
            current = self.state(entity).get('state')
            if door_rang(entity, self.door_states[index], current):
                self.emit(f'RING|{index}')
            if current not in (None, 'unavailable', 'unknown'):
                self.door_states[index] = current

    # ----- commands -----
    def execute(self, command):
        kind = command[0]
        try:
            if kind == 'E':
                if not self.elevator:
                    raise LookupError('elevator not configured')
                self.call(self.elevator['call'])
                self.elevator_called_at = self.clock()
                self.report_elevator()
            elif kind == 'O':
                if not self.office:
                    raise LookupError('office call not configured')
                self.call(self.office)
                self.office_until = self.clock() + OFFICE_CALL_SECONDS
                self.report_office()
        except Exception:
            self.emit({'E': 'ELEVERR', 'O': 'OFFICEERR'}[kind])

    def worker(self):
        poll_at = 0
        while not self.stop.is_set():
            try:
                self.execute(self.commands.get(timeout=0.05))
            except queue.Empty:
                pass
            if self.clock() >= poll_at:
                self.check_doors()
                self.report_elevator()
                self.report_office()
                poll_at = self.clock() + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--config', type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding='utf-8-sig'))
    token = os.environ.get('HOME_ASSISTANT_TOKEN', '')
    if not token:
        parser.error('Set HOME_ASSISTANT_TOKEN; never put a token in the source code')
    gateway = Gateway(config, token)
    import serial
    # Keep DTR/RTS low so opening the USB serial connection does not reset P4.
    port = serial.Serial()
    port.port = config.get('port', 'COM3')
    port.baudrate = 115200
    port.timeout = 0.05
    port.write_timeout = 1
    port.dtr = False
    port.rts = False
    port.open()
    worker = threading.Thread(target=gateway.worker, daemon=True)
    worker.start()
    ping_at = 0
    print('USB gateway running. Ctrl+C to stop. Unconfigured features stay unavailable.')
    try:
        buffer = bytearray()
        while True:
            if time.monotonic() >= ping_at:
                port.write(b'PING\n')
                ping_at = time.monotonic() + 2
            while not gateway.outgoing.empty():
                try:
                    line = gateway.outgoing.get_nowait()
                except queue.Empty:
                    break
                port.write((line + '\n').encode('ascii'))
            buffer.extend(port.read(256))
            while b'\n' in buffer:
                raw, _, buffer = buffer.partition(b'\n')
                command = parse_command(raw.decode('ascii', 'ignore').strip())
                if command:
                    try:
                        gateway.commands.put_nowait(command)
                    except queue.Full:
                        gateway.emit({'E': 'ELEVERR', 'O': 'OFFICEERR'}[command[0]])
            if len(buffer) > 1024:
                buffer.clear()
    except KeyboardInterrupt:
        pass
    finally:
        gateway.stop.set()
        port.close()


if __name__ == '__main__':
    main()
