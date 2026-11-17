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
│   └ 인터넷: NTP(시간), Open-Meteo(날씨), ip-api(위치 추정),          │
│            Nominatim(지명·검색), Esri(지도 타일)                  │
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
| `wallpad_link.cpp/.h` | 네트워크(WiFi/이더넷), EW11 TCP 연결, SDS 패킷 해석, 장치 상태, 명령 송신(화이트리스트, 장치별 마지막 값 대기열, 간격·횟수 제한) |
| `weather.cpp/.h` | 위치(저장값 → 설정값 → IP 추정), NTP, Open-Meteo 예보(ECMWF, 실패 시 2→30분 간격), 지명·장소 검색, 일식·월식 탐색, 해·달 경로(뜰 때~질 때 한 번, 72점: 해는 오늘, 달은 지금/다음) |
| `astro.cpp/.h` | 해·달(Meeus 저정밀 급수)·행성(JPL 근사 요소, 세차 보정) 위치, 방위·고도, 뜨고 지는 시각, 달 위상, 일식·월식 탐색과 그리기용 기하 |
| `geo_map.cpp/.h` | 위치 설정 지도: Esri 도로(≤13단계)/위성(14~17) 타일 JPEG를 하드웨어로 풀고, 위성 위에 도로·지명 PNG(ROM miniz로 해제)를 합성, PSRAM 40장 캐시. TLS 연결 재사용 |
| `board_config.h` | 화면·핀 상수, `local_config.h`(비공개) 포함 |
| `panel_variant.h`, `panel_select.h` | 패널 New/Old 선택(`panel_select.h`는 `build.ps1`이 생성, git 제외) |
| `src/lcd/` | 제조사 JD9165 드라이버(Apache-2.0). v3 칩용 DSI 클럭 수정 포함 |

## 태스크와 데이터 흐름

```
코어 0: wallpad(우선순위 4), weather(1), map(1) — 코어 1: UI 루프(Arduino loopTask)만.

[wallpad 태스크]  EW11 TCP 수신 → 패킷 분리(체크섬) → 상태(shared) 갱신 ─┐   (명령은 30ms 조용한 빈 슬롯에 송신,
                                                                       │    우리 패킷 직후의 응답만 성공으로 인정,
                                                                       │    UI는 기다리지 않음: 대기열에 장치별 마지막 값)
[weather 태스크]  NTP(부팅 즉시) → 해·달 경로(지면 다음 것) → 위치 → Open-Meteo → WeatherData ┤
                 (검색·IP·위치 변경 요청 처리, 하루 한 번 일식·월식 3년치 탐색)        │
[map 태스크]     지도 화면이 요청한 타일 → HTTPS(연결 재사용) → JPEG/PNG → 캐시 ──────┤
[loop (UI)]  250ms마다 스냅샷 복사 → 서명(signature) 비교 → 바뀌었으면 dirty ─────────┘
             터치/시리얼 처리 → dirty일 때만 render()
```

- **스냅샷 방식**: 태스크는 공유 구조체를 임계 구역(`portENTER_CRITICAL`) 또는 뮤텍스로 갱신하고,
  UI는 복사본만 읽습니다. UI는 네트워크를 직접 기다리지 않습니다.
- **다시 그리기 조건**: 내용 서명(방 상태·장치 상태·날씨 갱신 시각 등)이 바뀌거나, 분이 바뀌거나, 터치/알림이 있을 때.
  정지 화면은 다시 그리지 않습니다(PSRAM 대역폭 → 깜빡임 방지).
- **화면 그리기**: `panelAcquireBack`(이전 화면이 아직 나가는 중이면 그때만 대기) → 페이지별 "배경+유리 카드" 저장
  그림을 AXI DMA로 복사(`glassBegin`; 카드 구성이 바뀌면 그 프레임만 직접 그리고 저장) → 글자·값·아이콘 → 오버레이
  (가스 확인, 방문 호출, 알림) → 누름 표시 → `panelPresent`(기다리지 않음).
- **하늘 상태**: 해·달·행성·식 상태는 `skyNow()`가 1초에 한 번 계산해 홈 카드와 날씨 탭이 같이 씁니다.
  해·달 경로와 뜨고 지는 시각은 weather 태스크(코어 0)가 미리 계산합니다(UI 코어에서는 체험 모드 날짜만, 한 번).
- **측정**: 시리얼 `PERF`(그리기 구간별 시간, 루프 멈춤, 메모리, 스택, 지도 타일 통계).

## 화면 자원 파이프라인

```
tools/make_assets.py (배경 사진 처리, 아이콘 그리기) ─┐
tools/make_font.py   (UI 문구 → Noto Sans KR 4비트, 입력용 한글 전체) ─┤
tools/make_moon.py   (달 표면 지도 + 기복, 256~16px 밉맵) ────────────┴→ tools/assets_pack.py → assets/assets.bin (+ HomePanel/assets_data.h)
tools/upload_p4.py → 바뀌었을 때만 ffat 파티션(0x610000)에 기록 → 부팅 시 assets.cpp가 mmap
```

큰 데이터를 C 배열로 컴파일하지 않아 빌드가 빠르고 펌웨어가 작습니다(약 1MB).

## 외부 서비스와 개인정보

| 서비스 | 보내는 것 | 비고 |
|---|---|---|
| ip-api.com (HTTP) | 공인 IP(요청 자체) | 위치를 지정하지 않았을 때만. 통신사 위치가 나오는 경우가 많음 |
| NTP (pool.ntp.org 등) | 없음 | 시간 동기화 |
| api.open-meteo.com (HTTPS, 인증서 검증 생략) | 반올림 좌표 | 키 불필요, 30분마다 |
| nominatim.openstreetmap.org (HTTPS) | 지도에서 고른 좌표, 검색어 | 위치를 바꿀 때 한 번(지역 이름), 검색할 때 |
| server.arcgisonline.com (Esri, HTTPS) | 지도 타일 번호(보는 지역) | 위치 설정 화면을 열었을 때만. 화면에 `© Esri` 표기 |

비공개 값(EW11 주소, WiFi 이름·비밀번호, 날씨 위치)은 `HomePanel/local_config.h`에만 두며 git에 올리지 않습니다.
패널에서 고른 위치는 기기 플래시(NVS `location`)에만 저장됩니다.
