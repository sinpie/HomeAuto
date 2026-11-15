"""Upload an already rebuilt P4 v3.x application with the fixed esptool.

Espressif documents the P4 v3.2 stub issue and esptool==5.3.dev3 fix:
https://docs.espressif.com/projects/esp-faq/en/latest/development-environment/firmware-update.html
The tool is installed in a project-local virtual environment; Arduino is unchanged.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXED_VERSION = '5.3.dev3'


def image_paths(panel):
    build = ROOT/f'build-{panel}'
    options = json.loads((build/'build.options.json').read_text(encoding='utf-8-sig'))
    if 'ChipVariant=postv3' not in options.get('fqbn', ''):
        raise ValueError('Existing firmware targets pre-v3 silicon. Rebuild using updated build.ps1 first.')
    config = (build/'sdkconfig').read_text(encoding='utf-8')
    if 'CONFIG_ESP32P4_REV_MAX_FULL=399' not in config:
        raise ValueError('Build does not confirm compatibility with P4 revision v3.2.')
    # Use the Arduino-provided offsets from the successful build.
    lines = (build/'flash_args').read_text(encoding='utf-8').splitlines()
    expected = ['0x2000', '0x8000', '0xe000', '0x10000']
    actual = [line.split()[0] for line in lines[1:] if line.strip()]
    if actual != expected:
        raise ValueError('Unexpected flash layout; upload stopped.')
    cores = [Path(p) for p in options.get('hardwareFolders', '').split(',')]
    boot_app = next((p/'tools'/'partitions'/'boot_app0.bin' for p in cores
                     if (p/'tools'/'partitions'/'boot_app0.bin').is_file()), None)
    if boot_app is None:
        raise FileNotFoundError('Arduino boot_app0.bin not found')
    images = [('0x2000', build/'HomePanel.ino.bootloader.bin'),
              ('0x8000', build/'HomePanel.ino.partitions.bin'),
              ('0xe000', boot_app), ('0x10000', build/'HomePanel.ino.bin')]
    for _, path in images:
        if not path.is_file():
            raise FileNotFoundError(path)
    return images


def ensure_tool():
    environment = ROOT/'.tools'/'esptool-p4'
    python = environment/'Scripts'/'python.exe'
    if not python.is_file():
        subprocess.run([sys.executable, '-m', 'venv', str(environment)], check=True)
    result = subprocess.run([str(python), '-c',
        'import importlib.metadata; print(importlib.metadata.version("esptool"))'],
        capture_output=True, text=True)
    if result.returncode != 0 or result.stdout.strip() != FIXED_VERSION:
        print(f'Installing esptool {FIXED_VERSION} in the project. Internet is needed once.', flush=True)
        subprocess.run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check',
                        '--no-input', f'esptool=={FIXED_VERSION}'], check=True)
    return python


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--panel', choices=['New', 'Old'], required=True)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--no-stub', action='store_true')
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    try:
        images = image_paths(args.panel)
        if args.check_only:
            print('P4 v3.x build and flash layout validated. No installation or upload performed.')
            return 0
        python = ensure_tool()
        command = [str(python), '-m', 'esptool', '--chip', 'esp32p4',
                   '--port', args.port, '--baud', str(args.baud),
                   '--before', 'default-reset', '--after', 'hard-reset']
        if args.no_stub:
            command += ['--no-stub']
        command += ['write-flash', '--flash-mode', 'keep', '--flash-freq', 'keep', '--flash-size', 'keep']
        for address, path in images:
            command += [address, str(path)]
        # esptool checks hashes after writing; no mass erase or force option.
        return subprocess.run(command, cwd=ROOT).returncode
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print('Upload stopped:', error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
