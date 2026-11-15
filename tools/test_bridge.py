import unittest
from unittest.mock import Mock
from ha_bridge import Gateway, door_rang, parse_command, parse_floor, validate_config


def drain(gateway):
    lines = []
    while not gateway.outgoing.empty():
        lines.append(gateway.outgoing.get_nowait())
    return lines


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.config = {
            'base_url': 'http://homeassistant.local:8123',
            'office_call': 'script.call_office',
            'elevator': {'floor': 'sensor.elevator_floor', 'call': 'button.elevator_call', 'home_floor': 12},
            'doors': ['binary_sensor.front_door_bell', 'event.lobby_call', None],
        }
        self.now = [100.0]

    def gateway(self, **changes):
        return Gateway(dict(self.config, **changes), 'test', clock=lambda: self.now[0])

    def test_command_allowlist(self):
        self.assertEqual(parse_command('CMD|E|call'), ('E', 'call'))
        self.assertEqual(parse_command('CMD|O|call'), ('O', 'call'))
        for line in ['CMD|B|on', 'CMD|B|set|235', 'CMD|D|open', 'CMD|E|call|x', 'CMD|0|off']:
            self.assertIsNone(parse_command(line))

    def test_rejects_unsupported_entities(self):
        for changes in [{'office_call': 'lock.door'},
                        {'elevator': {'floor': 'sensor.f', 'call': 'lock.x', 'home_floor': 3}},
                        {'elevator': {'floor': 'sensor.f', 'call': 'button.x', 'home_floor': 0}},
                        {'doors': ['lock.front', None, None]}]:
            with self.assertRaises(ValueError):
                validate_config(dict(self.config, **changes))

    def test_floor_parsing(self):
        self.assertEqual(parse_floor('12'), 12)
        self.assertEqual(parse_floor('B2'), -2)
        for state in ['unknown', '0', 'B0', '', None, '1000']:
            self.assertEqual(parse_floor(state), 0)

    def test_door_ring_needs_a_change(self):
        self.assertFalse(door_rang('binary_sensor.a', None, 'on'))  # first read is not a call
        self.assertTrue(door_rang('binary_sensor.a', 'off', 'on'))
        self.assertFalse(door_rang('binary_sensor.a', 'on', 'on'))
        self.assertTrue(door_rang('event.a', '2026-11-15T10:00:00', '2026-11-15T10:05:00'))
        self.assertFalse(door_rang('event.a', '2026-11-15T10:00:00', 'unavailable'))

    def test_elevator_call_reports_calling_until_arrival(self):
        gateway = self.gateway()
        floors = iter(['3', '3', '12'])
        gateway.request = Mock(side_effect=lambda path, payload=None:
                               {} if payload is not None else {'state': next(floors)})
        gateway.execute(('E', 'call'))
        self.assertEqual(gateway.request.call_args_list[0].args,
                         ('services/button/press', {'entity_id': 'button.elevator_call'}))
        self.assertEqual(drain(gateway), ['ELEV|3|0|1'])
        gateway.report_elevator()
        gateway.report_elevator()
        self.assertEqual(drain(gateway), ['ELEV|3|0|1', 'ELEV|12|1|0'])

    def test_failures_never_report_success(self):
        gateway = self.gateway()
        gateway.request = Mock(side_effect=TimeoutError)
        gateway.execute(('E', 'call'))
        gateway.execute(('O', 'call'))
        self.assertEqual(drain(gateway), ['ELEVERR', 'OFFICEERR'])

    def test_unconfigured_features_do_nothing(self):
        gateway = self.gateway(office_call=None, elevator=None)
        gateway.request = Mock()
        gateway.execute(('E', 'call'))
        gateway.execute(('O', 'call'))
        gateway.request.assert_not_called()
        self.assertEqual(drain(gateway), ['ELEVERR', 'OFFICEERR'])

    def test_office_call_expires(self):
        gateway = self.gateway()
        gateway.request = Mock(return_value={})
        gateway.execute(('O', 'call'))
        self.assertEqual(drain(gateway), ['OFFICE|1'])
        self.now[0] += 31
        gateway.report_office()
        self.assertEqual(drain(gateway), ['OFFICE|0'])


if __name__ == '__main__':
    unittest.main()
