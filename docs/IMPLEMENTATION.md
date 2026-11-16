# 구현 가이드 — 처음부터 다시 만든다면

이 문서는 같은 패널을 새로 만들 때 필요한 순서, 결정, 그리고 실제로 부딪힌 함정을 정리합니다.
구조는 [ARCHITECTURE.md](ARCHITECTURE.md), 버스 규칙은 [RS485_PROTOCOL.md](RS485_PROTOCOL.md), 작업 지침은 [../AGENTS.md](../AGENTS.md).

## 1. 하드웨어와 기본값

| 항목 | 값 |
|---|---|
| 보드 | GUITION JC1060P470C_I_W_Y (ESP32-P4 v3.2 + ESP32-C6 WiFi 보조 칩) |
| 화면 | 7인치 1024×600 MIPI-DSI, JD9165, **New 패널**(제조사 New/Old 초기화가 다름) |
| 터치 | GT911, I2C SDA 7 / SCL 8, reset·interrupt 없음 |
| LCD | reset GPIO5, 백라이트 GPIO23 |
| 이더넷 | 내장 EMAC + IP101 PHY (MDC 31, MDIO 52, reset 51, RMII 클럭 입력 GPIO50) |
| RS485(보드) | UART1 TX 26 / RX 27, 자동 방향 전환 트랜시버(이 프로젝트는 EW11을 씀) |
| 플래시 | 16MB, 파티션 `16M Flash (3MB APP/9.9MB FATFS)` — 앱 0x10000, `ffat` 0x610000 |
| 툴체인 | Arduino-ESP32 3.3.7, arduino-cli(Arduino IDE 2 동봉), esptool 5.3.dev3 |

## 2. 처음 켤 때 막히는 것들 (순서대로)

1. **업로드 실패(stub 이후 속도 변경)**: IDE 동봉 esptool 5.1.0은 P4 v3.2에서 실패. 프로젝트 가상환경에 esptool 5.3.dev3.
2. **보드 옵션**: `ChipVariant=postv3`(v3 칩), PSRAM 켜기, USB CDC on boot.
3. **부팅 직후 abort 반복(화면 없음)**: 제조사 드라이버의 `MIPI_DSI_PHY_CLK_SRC_DEFAULT`는 3.3.7에서 v3 이전용 `PLL_F20M`.
   `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT`(XTAL)로 바꿔야 함. 크래시 주소는 `riscv32-esp-elf-addr2line`으로 해석.
4. **업로드 중 포트가 사라짐**: 앱이 재시작을 반복하면 USB가 계속 끊김 → esptool `--before usb-reset`.
5. **깜빡임**: 매번 전체 화면을 PSRAM→PSRAM 복사하지 말 것. 드라이버 프레임 버퍼 2개(`num_fbs=2`)에 그리고 vsync에 전환, 바뀔 때만 그리기.
6. **파란 노이즈**: RGB565 블렌딩을 R/B를 묶어 곱하면 넘침. 채널별로 계산.
7. **화면의 흐릿한 시계·WiFi 아이콘**: 출고 데모(esp-brookesia) 화면의 패널 잔상. 앱과 무관.

## 3. 구현 순서

1. **화면만**: 패널 초기화, 단색 → 사각형 → 글자. 시리얼로 화면 캡처(`SHOT`)부터 만들면 이후 모든 확인이 쉬워짐.
2. **렌더러**(`gfx`): 알파 블렌딩, SDF 둥근 사각형(AA), 4비트 글꼴·아이콘 그리기, 글래스 카드.
3. **자원 파이프라인**: 글꼴은 UI 문구에서 필요한 글자만 래스터화, 아이콘은 직접 그리기, 배경은 사진 처리 →
   하나의 blob을 `ffat` 파티션에 쓰고 mmap. (C 배열로 넣으면 빌드가 10분 이상 느려짐)
4. **네트워크**: WiFi(C6, ESP-Hosted)와 이더넷 중 선택. **부팅 직후 첫 `WiFi.begin()`은 C6 연결 준비 전이라 무시될 수 있으니 재시도**.
5. **버스 수신**: EW11 TCP 연결 → 패킷 분리 → 장치 상태. 조용하면(30초) 재연결. 오래된 값은 만료.
6. **UI 페이지**: 홈, 날씨, 보일러(방 4개), 제어(장치별 카드, 응답 있는 장치만), 방문 호출, 설정.
7. **명령 송신**: 화이트리스트 빌더 + 빈 슬롯 송신 + 응답 확인/재시도. 가스 잠금은 확인 팝업.
8. **인터넷**: 위치(설정값, 없으면 IP 추정) → NTP(시간대) → Open-Meteo(HTTPS). 해 고도·달 위상은 기기에서 계산.
   IP 위치는 통신사 장비 위치라 틀리기 쉬움(수원 → 서울 용산구). 자동 감지를 원하면 WiFi 측위(주변 AP MAC을
   외부 서비스로 전송: Google Geolocation API 키 필요, 또는 무료 BeaconDB)뿐이므로 설정값을 권장.
   Open-Meteo는 평문 HTTP 요청이 실패했고 HTTPS로 동작함.

## 4. 버스 분석 방법 (새 집에 적용할 때)

1. EW11을 9600 8E1, TCP 서버로 설정하고 PC에서 `tools/rs485_log.py`로 **수신만** 기록.
2. 체크섬이 맞지 않으면: 속도(19200에서 비트가 두 번씩 보이면 실제는 9600), 패리티, **A/B 극성**을 의심.
3. `--mark`로 Enter 마커를 남기며 한 가지씩 조작(예: 거실 켜기 → 25도 저장 → 23도 저장 → 끄기)해 바이트를 맞춤.
4. 응답하는 장치만 기록에 남음(`B0` 응답). 응답이 없는 헤더는 월패드가 스캔만 하는 것.
5. 송신은 수신 분석이 끝난 뒤, 상태가 바뀌어도 안전한 명령(이미 잠긴 가스 잠그기 등)으로 처음 시험.

## 5. 빌드와 업로드

```powershell
.\build.ps1 -Panel New                 # 컴파일만
.\build.ps1 -Panel New -Upload         # 컴파일 + 업로드(자원 blob은 바뀌었을 때만)
py -3 tools\upload_p4.py --panel New --assets always   # 자원까지 강제 업로드
```

- 파일 하나 수정 시 증분 빌드 약 1.7~3분, 전체 재빌드 약 13분(이 PC). 업로드 약 17초(자원 제외).
- **전체 재빌드가 되는 경우**: 스케치 파일 추가·삭제, 보드 옵션·`--build-property` 변경, 보드 패키지 `.txt` 변경, 빌드 중단.
  → 패널 선택은 `--build-property`가 아니라 생성 헤더(`panel_select.h`)로.
- 모든 파일이 포함하는 헤더(`board_config.h` 등)를 바꾸면 스케치 전체가 다시 컴파일됨.
- Windows Defender 제외(컴파일러 프로세스, Arduino15, 빌드 폴더)는 사용자가 직접 설정하면 크게 빨라짐.
- `arduino-cli --skip-libraries-discovery`는 라이브러리 링크까지 빠뜨려 쓸 수 없음.

## 6. 확인 방법

| 확인 | 방법 |
|---|---|
| 화면 | `py -3 tools\screenshot.py out.png [--tap X Y]` |
| 네트워크·WiFi 끊김 이유 | 시리얼 `NET` (`NET SCAN`은 주변 WiFi 개수·내 SSID 신호) |
| 장치 수신·명령 상태 | 시리얼 `DEV` |
| 자원 로드 | 시리얼 `ASSETS` |
| 버스 실제 송수신 | PC에서 `tools/rs485_log.py`로 동시에 기록 |
| PC 도구 | `py -3 -m unittest discover -s tools -p "test_*.py"` |

## 7. 설계 원칙

- **모르는 값은 모른다고 표시**(`--`, `미확인`). 통신 실패를 "꺼짐"으로 보이지 않기.
- **명령은 응답이 와야 성공**. 낙관적 갱신 금지.
- **버스 송신은 최소**: 화이트리스트, 한 번에 하나, 빈 슬롯, 재시도 상한.
- **비공개 정보는 `local_config.h`**: EW11 주소, WiFi 비밀번호. 위치는 IP 추정 후 반올림, 저장하지 않음.
- **디자인**: UI/UX Pro Max 스킬의 스마트홈 다크 시스템 기반. 사진 배경 위 글래스 카드, 강조색 하나(에메랄드),
  Noto Sans KR 4단계, 터치 영역 44px 이상, 누르면 강조색 테두리. 같은 종류의 카드는 같은 크기.
