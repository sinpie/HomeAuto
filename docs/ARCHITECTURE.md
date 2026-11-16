# 아키텍처

HomePanel은 7인치 ESP32-P4 보드(GUITION JC1060P470C_I_W_Y)에서 돌아가는 **월패드 보조 패널** 펌웨어입니다.
기존 삼성SDS EZON 월패드는 그대로 두고, 같은 RS485 버스를 EW11(RS485→TCP)로 듣고/제어하며,
인터넷에서 시간·날씨를 받아 한 화면에 보여 줍니다.

## 전체 구성

```
┌──────────── 집 안 ─────────────────────────────────────────────┐
│ 월패드(SHN-8070) ── RS485 9600 8E1 ── 각방 온도조절기(코텍), 가스, …   │
│                         │                                       │
│                       EW11 (RS485 ↔ TCP 서버, 포트 8899)         │
│                         │ WiFi/LAN                              │
│  HomePanel(ESP32-P4) ───┘          PC(선택): USB 시리얼 브리지     │
│   ├ WiFi: ESP32-C6 보조 칩(ESP-Hosted)  또는 유선 이더넷(IP101)     │
│   └ 인터넷: ip-api.com(위치), NTP(시간), Open-Meteo(날씨)          │
└─────────────────────────────────────────────────────────────────┘
```

- **월패드 버스(RS485)**: 난방·가스·조명·콘센트·환기·에너지 미터·현관 스위치·인터폰 신호. 패널이 듣고, 승인된 명령만 보냄.
- **USB 시리얼(선택)**: PC의 `tools/ha_bridge.py`가 Home Assistant와 연결해 엘리베이터 층, 관리실 호출,
  출입문 호출을 전달. 현재 이 집에서는 엘리베이터·관리실·로비 호출이 월패드의 이더넷(단지 서버) 쪽이라 RS485로는 오지 않음.
- **디버그**: 같은 USB 시리얼로 화면 캡처(`SHOT`), 터치 흉내(`TAP|x|y`), 상태 조회(`NET`, `DEV`, `ASSETS`).

## 펌웨어 모듈 (`HomePanel/`)

| 파일 | 역할 |
|---|---|
| `HomePanel.ino` | 비어 있음(진입점은 `app.cpp`; Arduino 자동 프로토타입 회피) |
| `app.cpp` | UI 상태, 페이지(홈·날씨·보일러·제어·방문 호출·설정), 터치, USB 시리얼 프로토콜, `setup()`/`loop()` |
| `gfx.cpp/.h` | 소프트웨어 렌더러: RGB565 알파 블렌딩, 안티앨리어싱 둥근 사각형(SDF), 4비트 글꼴·아이콘, 공용 색 |
| `panel.cpp/.h` | MIPI-DSI(JD9165) 패널, 프레임 버퍼 2개 + vsync 전환, 백라이트, GT911 터치(I2C) |
| `assets.cpp/.h`, `assets_data.h`, `font_data.h` | `ffat` 파티션의 화면 자원(배경·아이콘·글꼴)을 mmap. `assets_data.h`는 생성 파일(아이콘 ID) |
| `wallpad_link.cpp/.h` | 네트워크(WiFi/이더넷), EW11 TCP 연결, SDS 패킷 해석, 장치 상태, 명령 송신(화이트리스트) |
| `weather.cpp/.h` | IP 위치 조회, NTP, Open-Meteo 예보, 해 고도·달 위상 계산 |
| `board_config.h` | 화면·핀 상수, `local_config.h`(비공개) 포함 |
| `panel_variant.h`, `panel_select.h` | 패널 New/Old 선택(`panel_select.h`는 `build.ps1`이 생성, git 제외) |
| `src/lcd/` | 제조사 JD9165 드라이버(Apache-2.0). v3 칩용 DSI 클럭 수정 포함 |

## 태스크와 데이터 흐름

```
[wallpad 태스크]  EW11 TCP 수신 → 패킷 분리(체크섬) → 상태(shared) 갱신 ─┐   (명령은 빈 슬롯에 송신)
[weather 태스크]  위치 → NTP 설정 → 30분마다 Open-Meteo → WeatherData ──┤
[loop (UI)]  250ms마다 스냅샷 복사 → 서명(signature) 비교 → 바뀌었으면 dirty ┘
             터치/시리얼 처리 → dirty일 때만 render() → 숨은 버퍼에 그림 → vsync 전환
```

- **스냅샷 방식**: 태스크는 공유 구조체를 임계 구역(`portENTER_CRITICAL`) 또는 뮤텍스로 갱신하고,
  UI는 복사본만 읽습니다. UI는 네트워크를 직접 기다리지 않습니다.
- **다시 그리기 조건**: 내용 서명(방 상태·장치 상태·날씨 갱신 시각 등)이 바뀌거나, 분이 바뀌거나, 터치/알림이 있을 때.
  정지 화면은 다시 그리지 않습니다(PSRAM 대역폭 → 깜빡임 방지).
- **화면 그리기**: 배경(PSRAM 복사본) → 사이드바·헤더·엘리베이터 위젯 → 페이지 → 오버레이(가스 확인, 방문 호출) → 누름 표시.

## 화면 자원 파이프라인

```
tools/make_assets.py (배경 사진 처리, 아이콘 그리기) ─┐
tools/make_font.py   (UI 문구 → Noto Sans KR 4비트) ─┴→ tools/assets_pack.py → assets/assets.bin (+ HomePanel/assets_data.h)
tools/upload_p4.py → 바뀌었을 때만 ffat 파티션(0x610000)에 기록 → 부팅 시 assets.cpp가 mmap
```

큰 데이터를 C 배열로 컴파일하지 않아 빌드가 빠르고 펌웨어가 작습니다(약 1MB).

## 외부 서비스와 개인정보

| 서비스 | 보내는 것 | 비고 |
|---|---|---|
| ip-api.com (HTTP) | 공인 IP(요청 자체) | 위치를 지정하지 않았을 때만. 통신사 위치가 나오는 경우가 많음 |
| NTP (pool.ntp.org 등) | 없음 | 시간 동기화 |
| api.open-meteo.com (HTTPS, 인증서 검증 생략) | 반올림 좌표 | 키 불필요, 30분마다 |

비공개 값(EW11 주소, WiFi 이름·비밀번호, 날씨 위치)은 `HomePanel/local_config.h`에만 두며 git에 올리지 않습니다.
