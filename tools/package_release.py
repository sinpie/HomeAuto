from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
vendor_zip = root/'JC1060P470C_I_W_Y.zip'
with zipfile.ZipFile(vendor_zip) as source:
    license_path = ('JC1060P470C_I_W_Y/1-Demo/Demo_IDF/ESP-IDF_5.5.4/'
                    'JC1060P470C_I_W_Y_New_Panel/common_components/'
                    'espressif__esp32_p4_function_ev_board/LICENSE')
    license_bytes = source.read(license_path)
    if b'Apache License' not in license_bytes:
        raise ValueError('Expected the bundled Apache license')
    (root/'HomePanel'/'src'/'lcd'/'LICENSE-Apache-2.0.txt').write_bytes(license_bytes)
    for revision in ('New', 'Old'):
        vendor_path = (f'JC1060P470C_I_W_Y/1-Demo/Demo_Arduino/'
                       f'JC1060P470C_I_W_Y_{revision}_Panel/lvgl_demo_v8/src/lcd/esp_lcd_jd9165.c')
        actual = (root/'HomePanel'/'src'/'lcd'/f'jd9165_{revision.lower()}_impl.h').read_bytes()
        if actual != source.read(vendor_path):
            raise ValueError(f'{revision} vendor source changed')

files = list((root/'HomePanel').rglob('*'))
files += [root/n for n in ['README.md', 'build.ps1', 'Build-Check.cmd', 'Backup-And-Identify.cmd', 'Upload-New-Panel.cmd',
                           'Upload-Old-Panel.cmd', 'bridge.example.json']]
files += [root/'tools'/n for n in ['ha_bridge.py', 'test_bridge.py', 'make_font.py', 'diagnose_build.py',
                                'backup_identify.py', 'test_identify.py', 'upload_p4.py']]
target = root/'HomePanel_ESP32P4_source.zip'
with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as output:
    for path in files:
        if path.is_file():
            output.write(path, 'HomePanel_ESP32P4/'+path.relative_to(root).as_posix())
with zipfile.ZipFile(target) as output:
    assert output.testzip() is None
    assert not any(n.endswith(('.bin', 'bridge.json')) for n in output.namelist())
print(target)
print('Archive verified; exact vendor New/Old sources; no compiled firmware included.')
