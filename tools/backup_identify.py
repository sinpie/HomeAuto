"""Back up P4 flash and compare the application against supplied vendor images.

Read-only on flash. Does not select or upload a replacement firmware.
The local backup can contain device settings; no data is sent anywhere.
"""
import datetime
import hashlib
import json
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_OFFSET = 0x10000


def application(data, offset=APP_OFFSET):
    """Extract a complete ESP image, including checksum and optional SHA256."""
    if offset < 0 or offset + 32 > len(data) or data[offset] != 0xE9:
        return None
    count = data[offset+1]
    if not 1 <= count <= 16 or data[offset+32:offset+36] != b'\x32\x54\xcd\xab':
        return None
    pos = offset + 24
    for _ in range(count):
        if pos + 8 > len(data):
            return None
        length = struct.unpack_from('<I', data, pos+4)[0]
        pos += 8 + length
        if pos > len(data):
            return None
    # Checksum is the final byte of a 16-byte-aligned block.
    end = offset + ((pos-offset+16)//16)*16
    if data[offset+23]:
        end += 32
    return data[offset:end] if end <= len(data) else None


def identify(backup_data, archive_path):
    active = application(backup_data)
    report = {'application_offset': hex(APP_OFFSET), 'exact_matches': [],
              'panel_revision': 'unconfirmed'}
    if active is None:
        report['note'] = 'No ordinary unencrypted ESP application found at the expected offset.'
        return report
    report['application_sha256'] = hashlib.sha256(active).hexdigest()
    if not archive_path.is_file():
        report['note'] = 'Vendor ZIP is unavailable; backup was still saved.'
        return report
    with zipfile.ZipFile(archive_path) as archive:
        for entry in archive.infolist():
            if not entry.filename.endswith('.bin') or '/Burn files/' not in entry.filename:
                continue
            # Exclude the separate C6 wireless co-processor firmware.
            if 'C6-slave' in entry.filename:
                continue
            vendor = archive.read(entry)
            for offset in (APP_OFFSET, 0):
                candidate = application(vendor, offset)
                if candidate is not None and candidate == active:
                    report['exact_matches'].append(entry.filename)
                    break
    matches = report['exact_matches']
    if len(matches) == 1 and '_New_Panel.bin' in matches[0]:
        report['panel_revision'] = 'New (exact match to vendor New Panel application)'
    elif matches:
        report['note'] = 'Factory application matched, but the filename does not explicitly identify New/Old.'
    else:
        report['note'] = 'No exact application match. Do not infer panel revision from the device name.'
    return report


def main():
    from upload_p4 import ensure_tool
    python = ensure_tool()
    folder = ROOT/'device-backup'
    folder.mkdir(exist_ok=True)
    timestamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    target = folder/f'factory-p4-{timestamp}.bin'
    print('Reading COM3 flash to:', target, flush=True)
    result = subprocess.run([str(python), '-m', 'esptool', '--chip', 'esp32p4', '--port', 'COM3',
                             '--baud', '115200', 'read-flash', '0', 'ALL', str(target)])
    if result.returncode:
        print('Backup did not complete. No firmware was uploaded.')
        return result.returncode
    report = identify(target.read_bytes(), ROOT/'JC1060P470C_I_W_Y.zip')
    report['backup'] = str(target)
    output = folder/'identification.json'
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print('Backup complete. No firmware was uploaded.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
