# 폰트

| 용도 | 폰트 | 비고 |
|---|---|---|
| 본문·UI 전체 | **Pretendard Variable** | 한글 UI 기본. `font-sans` |
| 코드·IP·경로·버전 | **JetBrains Mono** | `font-mono`. IP / CIDR / 경로 / 버전 / 마스크는 전부 mono |

두 종만 씁니다. 시스템 폰트 폴백은 `globals.css` 에 이미 들어 있습니다.

**타입 스케일** (Figma Typography 컬렉션과 1:1)

| 토큰 | px | 쓰이는 곳 |
|---|---|---|
| `text-xs` | 11 | 헬프 텍스트, 캡션, 힌트 |
| `text-sm` | 12 | 표 셀, 배지, 폼 라벨 |
| `text-md` | 13 | 폼 값, 본문 조밀 |
| `text-base` | 14 | 본문 기본 |
| `text-lg` | 16 | 섹션 제목 |
| `text-xl` | 18 | 화면 제목 |
| `text-2xl`~`4xl` | 20·24·30 | KPI 숫자 |

문자 간격은 body 에 `-0.01em` 로 걸어뒀습니다. 개별로 다시 주지 마세요.
