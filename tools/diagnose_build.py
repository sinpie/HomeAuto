"""Read-only loader diagnostics and optional ordinary Arduino CLI compilation.

Does not change Windows mitigations, registry settings, DLLs, or compiler files.
Run Build-Check.cmd from Windows to compare with the restricted tool environment.
"""
import argparse
import ctypes
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
data_candidates = [Path(os.environ['LOCALAPPDATA'])/'Arduino15',
                   Path(os.environ['USERPROFILE'])/'AppData'/'Local'/'Arduino15']
DATA = next((p for p in data_candidates if (p/'packages'/'esp32').is_dir()), data_candidates[0])
TOOLS = DATA/'packages'/'esp32'/'tools'/'esp-rv32'/'2511'/'bin'


def aslr_policy():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.GetProcessMitigationPolicy.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                                 ctypes.c_void_p, ctypes.c_size_t]
    flags = ctypes.c_uint32()
    if not kernel.GetProcessMitigationPolicy(kernel.GetCurrentProcess(), 1,
                                             ctypes.byref(flags), 4):
        return {'error': ctypes.get_last_error()}
    return {'flags': flags.value, 'force_relocation': bool(flags.value & 2),
            'disallow_stripped_images': bool(flags.value & 8)}


def pe_info(path):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 60)[0]
    characteristics = struct.unpack_from('<H', data, pe+22)[0]
    optional = pe+24
    magic = struct.unpack_from('<H', data, optional)[0]
    directory = optional + (112 if magic == 0x20B else 96)
    address, size = struct.unpack_from('<II', data, directory + 5*8)
    return {'relocations_stripped': bool(characteristics & 1),
            'relocation_directory_rva': address, 'relocation_directory_bytes': size}


def launch_version(path):
    try:
        result = subprocess.run([str(path), '--version'], capture_output=True,
                                encoding='utf-8', errors='replace', timeout=15)
        return {'exit_code': result.returncode, 'output': (result.stdout+result.stderr)[:1200]}
    except OSError as error:
        return {'winerror': error.winerror, 'error': str(error)}
    except subprocess.TimeoutExpired:
        return {'error': 'Version query timed out'}


def compile_panels():
    candidates = [Path(os.environ.get('ProgramFiles', r'C:\Program Files'))/'Arduino IDE',
                  Path(os.environ['LOCALAPPDATA'])/'Programs'/'Arduino IDE']
    candidates = [p/'resources'/'app'/'lib'/'backend'/'resources'/'arduino-cli.exe'
                  for p in candidates]
    cli = next((p for p in candidates if p.is_file()), None)
    if cli is None:
        raise RuntimeError('Arduino CLI not found in the Arduino IDE installation')
    config = ROOT/'arduino-local.yaml'
    config.write_text('directories:\n  data: '+json.dumps(str(DATA))+
                      '\n  downloads: '+json.dumps(str(ROOT/'.cache'/'downloads'))+
                      '\n  user: '+json.dumps(str(ROOT/'.arduino'))+'\n', encoding='utf-8')
    fqbn = ('esp32:esp32:esp32p4:ChipVariant=postv3,USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,'
            'PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled')
    results = {}
    for panel, flag in [('Old', 0), ('New', 1)]:
        log = ROOT/f'compile-{panel}.log'
        args = [str(cli), 'compile', '--config-file', str(config), '--fqbn', fqbn,
                '--build-path', str(ROOT/f'build-{panel}'),
                '--build-property', f'compiler.c.extra_flags=-DHOME_PANEL_NEW={flag}',
                '--build-property', f'compiler.cpp.extra_flags=-DHOME_PANEL_NEW={flag}',
                str(ROOT/'HomePanel')]
        print(f'Compiling {panel} panel; log: {log}', flush=True)
        with log.open('wb') as output:
            result = subprocess.run(args, stdout=output, stderr=subprocess.STDOUT, cwd=ROOT)
        results[panel] = {'exit_code': result.returncode, 'log': str(log)}
        print(f'{panel}: '+('BUILD OK' if result.returncode == 0 else 'BUILD FAILED'), flush=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', action='store_true')
    args = parser.parse_args()
    policy = aslr_policy()
    context = 'restricted' if policy.get('disallow_stripped_images') else 'host'
    report = {'aslr_policy': policy, 'executables': {}}
    for tool in ['gcc', 'g++', 'as', 'ld']:
        path = TOOLS/f'riscv32-esp-elf-{tool}.exe'
        if not path.is_file():
            report['executables'][tool] = {'error': 'Not installed at expected path'}
            continue
        report['executables'][tool] = dict(pe_info(path), **launch_version(path))
        print(tool, report['executables'][tool], flush=True)
    tools_ok = all(v.get('exit_code') == 0 for v in report['executables'].values())
    if tools_ok and args.build:
        try:
            report['builds'] = compile_panels()
        except Exception as error:
            report['build_error'] = str(error)
    elif args.build:
        print('Compiler startup failed; compilation was not attempted.', flush=True)
    target = ROOT/f'toolchain-diagnostics-{context}.json'
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Report:', target, flush=True)
    return 0 if tools_ok else 1


if __name__ == '__main__':
    sys.exit(main())
