# HomePanel — GUITION JC1060P470C_I_W_Y

7인치 ESP32-P4 보드용 한글 월패드 보조 패널 앱입니다. 보드 제조사가 제공하는
`JC1060P470C_I_W_Y.zip`의 New/Old Panel Arduino 예제를 기준으로 합니다.
제조사 ZIP은 저작권과 용량(약 350MB) 때문에 저장소에 포함하지 않습니다. 보드 판매처에서 받으세요.

## 현재 상태

- **패널은 New로 확인되었습니다.** 칩은 ESP32-P4 v3.2(ROM eco7)이며 `ChipVariant=postv3`로 빌드합니다.
- 실물 업로드·부팅·화면·터치를 확인했습니다.
- 화면 크기는 제조사 예제의 **1024×600** 기준입니다. 상품 설명에 적힌 1027×768과 다릅니다.
- **보일러 상태는 월패드 RS485 버스에서 직접 읽습니다**(EW11 경유, 읽기 전용). 아래 "월패드 RS485 연동" 참고.
  WiFi로 연결해 방 4개의 켜짐·설정·현재 온도가 실시간 표시되는 것을 실물에서 확인했습니다.
- **WiFi:** 부팅 직후 첫 `WiFi.begin()`은 C6 보조 칩 연결(ESP-Hosted)이 준비되기 전이라 무시되는 경우가 있어,
  연결될 때까지 다시 시도합니다(첫 재시도 약 6초 후, 이후 20초 간격).
- **빌드 시간:** 파일 하나를 고친 증분 빌드 약 1.7분, 화면 자원이 그대로면 업로드 약 17초(이 PC 기준).
  전체 재빌드는 약 13분입니다. 아래 "빌드 시간 줄이기" 참고.
- Python 검사 16개(`tools/test_bridge.py`, `tools/test_identify.py`, `tools/test_rs485.py`)가 통과했습니다.

## 월패드 RS485 연동 (삼성SDS EZON 계열)

확인한 구성: 삼성SDS EZON SHN-8070 월패드, 코텍(Kotech) 각방 온도조절기 4개,
월패드 RS485 버스에 연결된 EW11(RS485→TCP 서버). 패널은 **WiFi 또는 유선 이더넷**으로 EW11에 접속해 **수신만** 합니다.

- 버스: **9600bps, 8E1**. 공개된 삼성SDS 패킷 규칙과 같습니다
  (패킷 첫 바이트 ≥ 0x80, 마지막 바이트 = 앞 바이트들의 XOR에서 bit7 제거).
- 난방: 월패드가 그룹 1~8에 `AE 7C <그룹> 00 00 00 00 <chk>`로 묻고, 온도조절기가
  `B0 7C <그룹> <켜짐 0/1> <설정온도> <현재온도> FF <chk>`로 답합니다(온도는 정수 ℃).
  그룹 1이 거실임을 설정 온도 스윕으로 확인했습니다. 그룹 2~4의 방 이름은 아직 미확인입니다.
- 현관 스위치(`AD`)와 조명(`AC`) 조회에는 응답하는 장치가 없고, 가스(`AB`)는 응답합니다.
- **EW11의 A/B 배선이 바뀌어 있으면** 9600bps에서도 의미 없는 22바이트 프레임만 보입니다.
  처음 이 집에서 그랬고, A/B를 바꾸자 모든 패킷의 체크섬이 맞았습니다.
- EW11 직렬 설정은 9600 / 8 / Even / 1로 둡니다(나중에 제어 명령을 보낼 때 패리티가 맞아야 함).

설정: `HomePanel/local_config.example.h`를 `HomePanel/local_config.h`로 복사해 값을 넣고 다시 빌드합니다.
이 파일은 git에 올라가지 않으므로 주소와 WiFi 비밀번호가 공개되지 않습니다.

| 항목 | 설명 |
|---|---|
| `WALLPAD_EW11_HOST` / `WALLPAD_EW11_PORT` | EW11 주소와 TCP 포트(기본 8899). 비어 있으면 패널에 `EW11 주소 설정 필요` |
| `WIFI_SSID` / `WIFI_PASSWORD` | 값이 있으면 WiFi로 연결. 비워 두면 보드의 LAN 포트(유선)를 씁니다 |

- WiFi는 보드의 ESP32-C6 보조 칩(ESP-Hosted)으로 동작하며 **2.4GHz 네트워크만** 연결됩니다.
- 유선은 보드의 IP101 PHY를 씁니다(MDC 31, MDIO 52, 리셋 51, RMII 클럭 입력 GPIO50, 제조사 예제 기준).
- 연결 상태는 화면 위쪽 상태 줄과 설정 화면의 `월패드` 줄에 표시됩니다(WiFi 신호 세기, IP, 받은 패킷 수).

패킷 기록·분석(수신 전용, PC에서):

```powershell
py -3 tools\rs485_log.py --host <EW11-IP> --seconds 120 --mark   # Enter로 동작 시점 표시
py -3 tools\rs485_log.py --analyze tools\rs485-logs\capture-xxx.log
```

기록 파일(`tools/rs485-logs/`)은 집 안의 실제 버스 데이터라 git에서 제외합니다.
버스에 명령을 보내는 기능(보일러 조작, 현관 스위치 흉내로 엘리베이터 호출)은 아직 구현하지 않았습니다.

### 실물에서 수정한 문제

- **부팅 직후 재시작 반복(화면 없음):** 제조사 드라이버의 `MIPI_DSI_PHY_CLK_SRC_DEFAULT`는
  보드 패키지 3.3.7에서 v3 이전 칩용 `PLL_F20M` 클럭을 가리키고, v3 칩 HAL은 이를 지원하지 않아 `abort()`합니다.
  `src/lcd/esp_lcd_jd9165.h`에서 `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT`(XTAL)로 바꿨습니다.
- **미세한 깜빡임:** 예전에는 1초마다 전체 화면(1.2MB)을 PSRAM에서 PSRAM으로 CPU 복사했습니다.
  지금은 드라이버 프레임 버퍼 2개를 두고 숨은 쪽에 그린 뒤 vsync에 전환하며, 내용이 바뀔 때만 다시 그립니다.
- **업로드 중 COM3 끊김:** 앱이 재시작을 반복하면 USB 포트가 계속 사라집니다.
  esptool에 `--before usb-reset`을 주면 업로드됩니다.
- 화면 위쪽의 흐릿한 시계·WiFi·배터리 잔상은 앱이 그리는 것이 아닙니다(코드·폰트에 없음).
  보호필름을 떼고 전원을 다시 켜도 남아 있어, 출고 데모 화면에 의한 패널 잔상으로 추정됩니다. 아직 확정되지 않았습니다.

### P4 v3.2 업로드 수정

실물 업로드 로그에서 ESP32-P4 revision v3.2, 400MHz, USB-Serial/JTAG가 확인되었습니다.
기존 esptool 5.1.0은 stub 실행 후 속도 변경 단계에서 실패했습니다.
Espressif FAQ는 이 리비전의 유사한 업로드 문제에 esptool 5.3.dev3 또는 4.12.dev2를 안내합니다.

수정된 `build.ps1`은 v3.00 이상 칩용으로 재빌드하고, `tools/upload_p4.py`를 통해
프로젝트의 `.tools/esptool-p4` 가상환경에 esptool **5.3.dev3**를 설치해 **921600bps**로 업로드합니다
(`--before usb-reset`: 앱이 재시작을 반복해 포트가 끊겨도 업로드됨).
첫 실행에는 인터넷이 필요합니다. 기존 Arduino 설치 및 포함된 esptool은 수정하지 않습니다.
v3 이전 설정으로 빌드된 파일은 업로드 전에 거부하도록 검사합니다.

근거: https://docs.espressif.com/projects/esp-faq/en/latest/development-environment/firmware-update.html

## 컴파일러 DLL 오류 진단

현재 작업 환경에서 `GetProcessMitigationPolicy`로 확인한 ASLR 플래그는 `0xF`입니다.
강제 재배치와 재배치 정보가 제거된 실행 파일 차단이 활성화되어 있습니다.
설치된 `esp-rv32/2511`의 gcc, g++, as, ld는 모두 PE 재배치 정보가 제거되어 있고,
버전 조회 단계에서 Windows 오류 623으로 실행이 실패했습니다.
이 조합은 현재 실행 환경과 설치된 컴파일러가 호환되지 않음을 보여 줍니다.
DLL 파일 누락이나 앱 소스의 컴파일 오류로 확인된 것이 아닙니다.

Windows 탐색기에서 `Build-Check.cmd`를 직접 실행하면 해당 PC의 일반 실행 환경에서
동일 컴파일러가 실행되는지 검사합니다. 정상 실행될 경우 New/Old 펌웨어를 각각 빌드하고
`compile-New.log`, `compile-Old.log`를 남깁니다. 장치에는 업로드하지 않습니다.
진단 결과는 `toolchain-diagnostics-host.json` 또는 `toolchain-diagnostics-restricted.json`에 저장됩니다.
이 스크립트는 Windows 보안 설정, 레지스트리, DLL 및 컴파일러 원본을 변경하지 않습니다.

관련 공개 보고: https://github.com/openai/codex/issues/51674

## 업로드 전 패널 버전 확인

`Backup-And-Identify.cmd`를 Windows 탐색기에서 직접 실행하면 COM3의 P4 플래시를
`device-backup/factory-p4-날짜.bin`으로 백업합니다. 시리얼 모니터와 브리지는 닫아 주세요.
플래시에 쓰지 않으며, 장치 재시작은 발생할 수 있습니다.

기존 앱을 제조사 ZIP의 출고 펌웨어와 바이트 단위로 비교해
`device-backup/identification.json`에 결과를 기록합니다. New Panel로 명시된 출고 앱과
정확히 일치할 때만 New로 표시합니다. 일치하지 않거나 파일명에 패널 버전 표시가 없으면
미확인으로 유지합니다. 기기 설정 영역은 비교에서 제외합니다.
백업에는 기존 네트워크 설정이 포함될 수 있으므로 PC에 보관하세요.

## 포함 기능과 제한

화면 구성: 왼쪽 메뉴(홈 / 보일러 / 방문 호출 / 설정), 오른쪽 위에 **엘리베이터 위젯 상시 표시**.

- **엘리베이터:** 현재 층(B1 등 지하 표시)과 이동 방향을 항상 표시합니다. 위젯의 `호출`을 누르면 호출합니다.
  상태: 호출 → 요청 중 → 호출됨 → 도착. 응답이 6초 안에 없으면 `실패`로 표시합니다.
- **보일러(읽기 전용):** 방 4개의 난방 켜짐/꺼짐, 설정 온도, 현재 온도를 월패드 버스에서 읽어 표시합니다.
  15초 동안 응답이 없으면 `미확인`으로 바꿉니다. 조작은 기존 온도조절기에서 합니다.
- **관리실 통화:** 관리실 호출 요청을 보냅니다. 패널에는 마이크·스피커 경로가 없어 음성 통화는 하지 않습니다.
- **방문 호출(일방):** 현관, 1층 현관, 지하주차장 문에서 호출이 오면 전체 화면 알림을 띄우고
  화면을 최대 밝기로 켭니다. 응답·문열림 기능은 없으며, 최근 8건을 `N분 전`으로 기록합니다.
- 엘리베이터·관리실·방문 호출 값은 USB 브리지가 보낸 것만 표시합니다. 연결 전에는 `--`, `미연결`로 표시하고 명령을 보내지 않습니다.
  명령은 서버가 수락했다는 응답이 와야 성공으로 표시합니다.
- 설정의 `체험 시작`은 화면에서만 동작하는 시뮬레이션입니다(엘리베이터가 12층으로 이동, `방문 호출 시험` 버튼).
- 화면 왼쪽 아래 `가동 N분`은 현재 시각이 아니라 부팅 후 경과 시간입니다. 패널에 시계(RTC/네트워크 시간)가 없습니다.
- 월패드 영상(인터폰 카메라)은 월패드 제어보드의 아날로그 VIDEO 단자로 들어오는 것으로 보이며, 이 패널에는 표시하지 않습니다.
- 디자인: 서울 야경 사진 배경 위 반투명(글래스) 카드, 에메랄드 강조색 한 가지, 선 아이콘 18종,
  Noto Sans KR 안티앨리어싱 글꼴 4단계(19/23/30/60px), 터치 영역 최소 56px, 누를 때 강조색 테두리로 반응을 표시합니다.

### 화면 자원 (배경·아이콘·글꼴)

화면 자원은 펌웨어에 컴파일하지 않습니다. 하나의 바이너리 `assets/assets.bin`(약 1.4MB)으로 묶어
플래시의 기존 `ffat` 데이터 파티션(0x610000, 9.9MB)에 따로 씁니다. 펌웨어는 부팅할 때 이 영역을
메모리에 매핑(mmap)해 복사 없이 씁니다(`HomePanel/assets.cpp`). 형식은 `tools/assets_pack.py` 맨 위 설명에 있습니다.

- 글꼴: `py -3 tools\make_font.py` — UI 문구를 바꾼 뒤 실행(Noto Sans KR, OFL-1.1). `assets.bin`을 다시 묶습니다.
- 배경·아이콘: `py -3 tools\make_assets.py --photo <사진.jpg>` — 처리된 배경을 `assets/background.png`로 저장하고 다시 묶습니다.
  현재 배경은 Unsplash `mtFSMhcqNpM`(https://unsplash.com/photos/mtFSMhcqNpM, Unsplash License)입니다.
  `--photo` 없이 실행하면 코드로 그린 야경 일러스트를 씁니다. 아이콘은 이 스크립트가 직접 그립니다.
- 업로드: `tools\upload_p4.py`가 `assets.bin`이 바뀌었을 때만 함께 씁니다(`--assets auto|always|never`).
  플래시를 지웠거나 보드를 바꿨으면 `--assets always`로 한 번 올립니다.
- 자원이 없으면 화면이 비고 시리얼에 `ERROR|assets|...`가 나옵니다. `ASSETS` 명령으로 상태를 확인할 수 있습니다.

### 디버그 명령 (USB 시리얼, 115200)

- `SHOT` / `TAP|x|y`: 화면 캡처와 터치 흉내. `py -3 tools\screenshot.py out.png [--tap X Y]`
- `NET`: 연결 방식, 상태, 받은 패킷 수, WiFi 상태와 **마지막 끊김 이유 코드**
  (15·204 = 비밀번호 오류 가능성 높음, 201 = AP 없음, 202 = 인증 실패).
- `NET SCAN`: 위 내용 + 주변 WiFi 개수와 설정한 SSID의 신호·채널(다른 SSID 이름은 출력하지 않음). 몇 초간 멈춥니다.
- `ASSETS`: 화면 자원 로드 결과.

## Arduino IDE에서 업로드

1. `HomePanel/HomePanel.ino`를 엽니다.
2. Espressif ESP32 보드 패키지 3.3.7을 기준으로 설정합니다. 제조사 예제는 3.1 초과 버전을 요구합니다.
3. 보드: **ESP32P4 Dev Module**, Chip Variant: **v3.00 or newer**, PSRAM: **Enabled**, Flash: **16MB**,
   Partition: **16M Flash (3MB APP/9.9MB FATFS)**,
   USB Mode: **Hardware CDC and JTAG**, USB CDC On Boot: **Enabled**.
4. 패널 버전은 기본값이 New입니다(이 프로젝트 보드에서 확인). 구형이면 `HomePanel/panel_select.h`에
   `#define HOME_PANEL_NEW 0`을 둡니다(`build.ps1 -Panel Old`가 자동으로 만듭니다).
   화면 자원은 IDE로 올라가지 않으므로 `py -3 tools\upload_p4.py --panel New --assets always`로 한 번 올립니다.
5. IDE 포함 esptool 5.1.0으로는 이 칩의 업로드가 실패했으므로, 업로드에는 아래 수정 스크립트를 사용합니다.
6. 화면 출력·터치 좌표·밝기를 실물에서 확인합니다. 다른 패널 버전을 무작정 연속 업로드하지 말고
   판매자 표기나 패널 모델을 확인하는 것이 좋습니다.

별도의 LVGL 라이브러리는 필요하지 않습니다. JD9165 드라이버와 한글 비트맵 글리프가 포함되어 있습니다.

## PowerShell에서 빌드·업로드

신형 패널:

```powershell
.\build.ps1 -Panel New -Upload -Port COM3
```

구형 패널:

```powershell
.\build.ps1 -Panel Old -Upload -Port COM3
```

`-Upload`를 생략하면 컴파일만 합니다. 스크립트는 빌드가 실패하면 업로드를 실행하지 않습니다.
`Upload-New-Panel.cmd` / `Upload-Old-Panel.cmd`도 같은 작업을 실행합니다.
PowerShell 정책 때문에 스크립트가 실행되지 않으면 Arduino IDE를 이용하세요.

### 빌드 시간 줄이기

같은 빌드 폴더(`build-New`)를 다시 쓰면 바뀐 파일만 다시 컴파일합니다. 라이브러리 탐색도 파일별로 캐시됩니다.
(`arduino-cli --skip-libraries-discovery`는 링크할 라이브러리까지 빠뜨려 쓸 수 없습니다.)

- **빌드 폴더가 통째로 지워지는 경우**(전체 재빌드, 이 PC에서 약 13분): 스케치 파일 목록이 바뀌었을 때, `--build-property`나 보드 옵션이
  바뀌었을 때, ESP32 보드 패키지 폴더의 `.txt` 파일이 바뀌었을 때. 빌드 중간에 멈추는 것도 피하세요.
- 패널 선택은 컴파일 옵션이 아니라 `panel_select.h` 파일로 합니다. 컴파일 옵션을 쓰면 ESP32 코어(57개 파일)를 매번 다시 빌드합니다.
- 큰 데이터(배경·글꼴)는 컴파일하지 않고 `assets.bin`으로 따로 올립니다.
- **Windows Defender 제외 설정**(직접 설정해야 함, 보안 설정이므로 신중히): 컴파일러가 파일을 열 때마다 검사되어
  매우 느려집니다. 보고된 사례에서 3~6배 빨라졌습니다.
  - 폴더: `%LOCALAPPDATA%\Arduino15\packages\esp32`, `%LOCALAPPDATA%\arduino`, 프로젝트의 `build-New`·`build-Old`
  - 프로세스: `cc1plus.exe`, `cc1.exe`, `riscv32-esp-elf-g++.exe`, `riscv32-esp-elf-gcc.exe`, `as.exe`, `ld.exe`,
    `collect2.exe`, `arduino-cli.exe`, `esptool.exe`, `gen_esp32part.exe`

## 선택 사항: Home Assistant USB 브리지

실제 Home Assistant 서버에 보일러·엘리베이터·호출 장치가 연동되어 있을 때만 사용합니다.

1. `bridge.example.json`을 `bridge.json`으로 복사하고 서버 주소와 엔티티를 입력합니다. 쓰지 않는 항목은 `null`로 둡니다.

   ```json
   {
     "port": "COM3",
     "base_url": "http://homeassistant.local:8123",
     "boiler": "climate.boiler",
     "office_call": "script.call_management_office",
     "elevator": {"floor": "sensor.elevator_floor", "call": "button.elevator_call", "home_floor": 12},
     "doors": ["binary_sensor.front_door_bell", "event.lobby_call", "binary_sensor.parking_door_bell"]
   }
   ```

   - `boiler`: `climate.*`만 허용합니다.
   - `office_call`, `elevator.call`: `script.*` 또는 `button.*`만 허용합니다.
   - `elevator.floor`: `sensor.*`. 상태가 `12`, `B1` 같은 층 값이어야 합니다. `home_floor`(우리 집 층)에 도착하면 호출이 끝난 것으로 봅니다.
   - `doors`: 현관, 1층 현관, 지하주차장 문 순서입니다. `binary_sensor.*`는 꺼짐→켜짐, `event.*`는 새 이벤트를 호출로 봅니다.
2. Python에 `pyserial`을 설치합니다.
3. 시리얼 모니터를 닫습니다. 브리지가 COM3를 독점 사용합니다.
4. 현재 터미널에서만 토큰을 설정하고 실행합니다. 토큰을 소스나 JSON에 기록하지 마세요.

```powershell
$env:HOME_ASSISTANT_TOKEN = Read-Host 'Home Assistant token'
python tools\ha_bridge.py --config bridge.json
```

시리얼 프로토콜은 `tools/ha_bridge.py` 맨 위 설명에 있습니다. 통신 오류나 설정되지 않은 기능은 꺼짐으로 간주하지 않습니다.
가정 내 신뢰할 수 있는 망에서 사용하고, 원격 연결에는 유효한 인증서가 있는 HTTPS 주소를 사용하세요.

## 참고 자료와 변경점

- 제조사 ZIP의 `1-Demo/Demo_Arduino/JC1060P470C_I_W_Y_{New,Old}_Panel/lvgl_demo_v8/`.
- LCD reset GPIO5, backlight GPIO23(제조사 `jd9165_lcd.cpp` 기준), touch SDA7/SCL8.
- 터치 reset/interrupt GPIO는 제조사 설정대로 사용하지 않습니다.
- 터치 컨트롤러의 실제 좌표 범위는 GT911 설정 레지스터에서 읽습니다.
- Old: 52MHz, H sync/back/front 20/160/160, V 10/23/12.
- New: 52MHz, H 24/136/160, V 2/21/12.
- `jd9165_old_impl.h`, `jd9165_new_impl.h`는 제조사 ZIP의 해당 C 드라이버 원문입니다.
  Arduino가 빌드 폴더에 포함하도록 `.h` 확장자를 사용합니다.
  드라이버의 Espressif Apache-2.0 고지를 보존합니다.
- 앱은 PSRAM에 RGB565 버퍼를 할당하고 동기 복사로 표시합니다. 실제 패널에서
  터치 방향·주사 중 화면 갱신·프레임 성능은 아직 확인하지 못했습니다.

## 검사

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
```
