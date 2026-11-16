# AGENTS.md — 이 저장소에서 일하는 사람과 AI 에이전트를 위한 지침

HomePanel: ESP32-P4 7인치 월패드 보조 패널(Arduino-ESP32 3.3.7). 개요는 [README.md](README.md),
확인된 사실·결정·이력은 [MEMORY.md](MEMORY.md), 구조는 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), 새로 만들 때의 순서·함정은 [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md),
버스 규칙은 [docs/RS485_PROTOCOL.md](docs/RS485_PROTOCOL.md).

## 반드시 지킬 것

1. **월패드 버스 송신은 사용자 승인 범위 안에서만.** 현재 승인된 명령: 난방 켜기/끄기·설정 온도, 가스 잠그기,
   조명·콘센트 켜기/끄기, 환기. 현관 스위치 흉내(엘리베이터 호출), 인터폰 문 열기, 미확인 헤더 탐색은 **사용자에게 먼저 물을 것**.
   새 명령은 `wallpad_link.cpp`의 빌더 함수로만 추가하고 `client.write`를 다른 곳에 두지 말 것.
2. **비공개 정보를 커밋하지 말 것.** EW11 주소·WiFi 이름/비밀번호·날씨 위치는 `HomePanel/local_config.h`(git 제외).
   버스 기록(`tools/rs485-logs/`), 플래시 백업, 제조사 ZIP, 빌드 산출물도 제외. 커밋 전 `git grep`으로 IP·이메일·MAC 확인.
   커밋 작성자 이메일은 GitHub no-reply 주소.
3. **모르는 값은 모른다고 표시**하고, 명령 성공은 장치 응답으로만 판단할 것.
4. **실기기에서 확인**: 화면은 `tools/screenshot.py`로 캡처해서 보고, 상태는 시리얼 `NET`/`DEV`/`ASSETS`로 확인.
   확인하지 못한 것은 확인하지 못했다고 보고.

## 빌드·업로드

```powershell
.\build.ps1 -Panel New -Upload
```

- 빌드는 오래 걸림(증분 2~3분, 전체 약 13분). 백그라운드로 돌리고 **빌드 중에는 스케치 소스를 고치지 말 것**(경쟁 상태).
- **빌드 중간에 멈추지 말 것**: 빌드 폴더가 무효화되어 다음이 전체 재빌드가 됨.
- 스케치 파일 추가·삭제는 전체 재빌드를 부름 → 묶어서 한 번에.
- `--build-property`를 쓰지 말 것(코어 캐시 무효화). 설정은 생성 헤더로.
- 업로드: `tools/upload_p4.py`(esptool 5.3.dev3, 921600bps, `--before usb-reset`). 자원 blob은 바뀌었을 때만 씀.
- 큰 데이터(이미지·글꼴)를 C 배열로 넣지 말 것. `tools/assets_pack.py` → `assets/assets.bin` → `ffat` 파티션.

## 코드 규칙

- 진입점은 `app.cpp`(`HomePanel.ino`는 비움). 그리기 함수는 `gfx`, 네트워크·버스는 `wallpad_link`, 인터넷은 `weather`.
- 태스크 ↔ UI는 **스냅샷 복사**로만 주고받음(임계 구역/뮤텍스). UI 루프에서 블로킹 네트워크 호출 금지.
- 화면은 내용이 바뀔 때만 다시 그림(서명 비교). 정지 화면을 주기적으로 다시 그리지 말 것(깜빡임).
- 터치 영역 44px 이상, 같은 종류 카드는 같은 크기, 강조색은 하나. UI 문구를 바꾸면 `py -3 tools\make_font.py`.
- `millis()` 비교는 뺄셈으로(랩어라운드 안전). 오래 지나는 시각(방문 호출 등)은 64비트.
- 쉘에서 C++ 소스를 고칠 때 heredoc 안의 `\n`이 실제 줄바꿈으로 바뀌는 사고가 있었음 → 패치는 파일로 작성한 스크립트나 편집 도구로.

## 확인 명령 (USB 시리얼 115200)

| 명령 | 응답 |
|---|---|
| `SHOT` | 화면 원시 RGB565 (도구: `tools/screenshot.py`) |
| `TAP|x|y` | 터치 흉내 |
| `NET` / `NET SCAN` | 연결 방식·상태·패킷 수·WiFi 끊김 이유 / + 주변 WiFi 개수 |
| `DEV` | 장치별 수신 상태, 마지막 명령 상태 |
| `ASSETS` | 자원 로드 결과 |

## PC 도구

| 도구 | 용도 |
|---|---|
| `tools/rs485_log.py` | EW11에서 버스 수신 기록·분석(송신 안 함), `--mark`로 동작 시점 표시 |
| `tools/screenshot.py` | 화면 캡처, 터치 흉내 |
| `tools/assets_pack.py`, `make_font.py`, `make_assets.py` | 화면 자원 생성 |
| `tools/upload_p4.py` | 업로드(자원 포함) |
| `tools/ha_bridge.py` | 선택: Home Assistant ↔ 패널 USB 브리지(엘리베이터·관리실·방문 호출) |

테스트: `py -3 -m unittest discover -s tools -p "test_*.py"`
