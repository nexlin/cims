import type { Config } from 'tailwindcss'

/**
 * CIMS Console — Tailwind 설정
 *
 * 정본은 `cims-design-handoff/tokens/tailwind.config.ts` 이고, 우리 트리에 맞춰 세 곳만 바꿨다
 * (docs/design/console_design_system.md §2·§8 T0b):
 *   1. content 에 서비스 팩(`../../service/console/src`) 을 포함한다. 서비스 팩은 자체
 *      package.json 없이 여기 node_modules 를 링크로 공유하므로, 빠뜨리면 12개 화면의
 *      클래스가 전부 purge 된다.
 *   2. darkMode 를 `.dark` 클래스가 아니라 우리 테마 스위치(`:root[data-theme="dark"]`)에 건다.
 *   3. preflight(전역 reset)를 끈다 — index.css 의 reset 과 동시에 켜면 버튼·표 높이가
 *      어긋난다. index.css reset 을 걷어내는 T4 에서 켠다.
 *
 * 색은 여기서 새로 정의하지 않는다. 값은 전부 index.css 의 토큰(= Figma 01)에만 있다.
 */
const config: Config = {
  darkMode: ['selector', '[data-theme="dark"]'],
  content: [
    './index.html',
    './src/**/*.{ts,tsx}',
    '../../service/console/src/**/*.{ts,tsx}',
  ],
  corePlugins: { preflight: false },
  // 이행 중 충돌 차단 — 아래 이름은 **우리 레거시 클래스**인데 Tailwind 유틸리티와 겹친다.
  // 그대로 두면 Tailwind 가 조용히 스타일을 얹는다:
  //   text-muted (6곳) → color: var(--muted) = #f8fafc. 흰 배경에 흰 글씨가 된다.
  //                      우리 CSS 에 정의가 없어 지금은 무동작인 클래스다.
  //                      muted 글자는 Tailwind 에서 `text-muted-foreground` 가 맞다.
  //   table      (5곳) → display: table. 전부 <table> 엘리먼트라 지금은 무해하지만
  //                      의도한 적용이 아니므로 함께 막는다.
  // 해당 페이지를 T3 에서 옮기면 레거시 이름이 사라지므로 이 blocklist 도 걷는다.
  blocklist: ['text-muted', 'table'],
  theme: {
    extend: {
      colors: {
        background: 'var(--background)',
        foreground: 'var(--foreground)',
        card: { DEFAULT: 'var(--card)', foreground: 'var(--card-foreground)' },
        popover: { DEFAULT: 'var(--popover)', foreground: 'var(--popover-foreground)' },
        primary: { DEFAULT: 'var(--primary)', foreground: 'var(--primary-foreground)' },
        secondary: { DEFAULT: 'var(--secondary)', foreground: 'var(--secondary-foreground)' },
        muted: { DEFAULT: 'var(--muted)', foreground: 'var(--muted-foreground)' },
        accent: { DEFAULT: 'var(--accent)', foreground: 'var(--accent-foreground)' },
        destructive: { DEFAULT: 'var(--destructive)', foreground: 'var(--destructive-foreground)' },
        border: 'var(--border)',
        input: 'var(--input)',
        ring: 'var(--ring)',
        sidebar: {
          DEFAULT: 'var(--sidebar)',
          foreground: 'var(--sidebar-foreground)',
          primary: 'var(--sidebar-primary)',
          'primary-foreground': 'var(--sidebar-primary-foreground)',
          accent: 'var(--sidebar-accent)',
          'accent-foreground': 'var(--sidebar-accent-foreground)',
          border: 'var(--sidebar-border)',
          ring: 'var(--sidebar-ring)',
        },
        // CIMS 의미색 — shadcn 에 없는 것
        success: { DEFAULT: 'var(--cims-success)', soft: 'var(--cims-success-soft)', on: 'var(--cims-success-on-soft)' },
        warning: { DEFAULT: 'var(--cims-warning)', soft: 'var(--cims-warning-soft)', on: 'var(--cims-warning-on-soft)' },
        info: { DEFAULT: 'var(--cims-info)', soft: 'var(--cims-info-soft)', on: 'var(--cims-info-on-soft)' },
        neutral: { DEFAULT: 'var(--cims-neutral)', soft: 'var(--cims-neutral-soft)', on: 'var(--cims-neutral-on-soft)' },
        brandsoft: { DEFAULT: 'var(--cims-brand-soft)', on: 'var(--cims-brand-on-soft)' },
        dangersoft: { DEFAULT: 'var(--cims-danger-soft)', on: 'var(--cims-danger-on-soft)' },
        'border-strong': 'var(--cims-border-strong)',
        'text-disabled': 'var(--cims-text-disabled)',
      },
      borderRadius: { sm: '6px', md: '8px', lg: '14px' },
      fontSize: {
        // Figma Typography 컬렉션 그대로
        xs: ['11px', { lineHeight: '1.4' }],
        sm: ['12px', { lineHeight: '1.4' }],
        md: ['13px', { lineHeight: '1.5' }],
        base: ['14px', { lineHeight: '1.5' }],
        lg: ['16px', { lineHeight: '1.4' }],
        xl: ['18px', { lineHeight: '1.3' }],
        '2xl': ['20px', { lineHeight: '1.3' }],
        '3xl': ['24px', { lineHeight: '1.25' }],
        '4xl': ['30px', { lineHeight: '1.25' }],
      },
      fontFamily: {
        // 실제 등록 이름 — npm `pretendard` / `@fontsource-variable/jetbrains-mono` 가
        // 각각 'Pretendard Variable' / 'JetBrains Mono Variable' 로 @font-face 를 건다.
        sans: ['"Pretendard Variable"', 'Pretendard', 'system-ui', 'sans-serif'],
        mono: ['"JetBrains Mono Variable"', 'ui-monospace', 'SFMono-Regular', 'Menlo', 'monospace'],
      },
      boxShadow: {
        sm: 'var(--cims-elevation-sm)',
        lg: 'var(--cims-elevation-lg)',
        focus: 'var(--cims-focus-ring)',
      },
      // Figma Spacing 컬렉션 (2·4·6·8·10·12·14) 중 기본 스케일에 없는 것
      spacing: { '2.5': '10px', '3.5': '14px' },
    },
  },
  plugins: [require('tailwindcss-animate')],
}

export default config
