import type { Config } from "tailwindcss";

/**
 * CIMS Console — Tailwind 설정
 * globals.css 의 CSS 변수를 Tailwind 유틸리티로 노출합니다.
 * 색을 여기서 새로 정의하지 마세요. 값은 전부 globals.css 에만 있습니다.
 */
const config: Config = {
  darkMode: ["class"],
  content: ["./index.html", "./src/**/*.{ts,tsx}"],
  theme: {
    extend: {
      colors: {
        background: "var(--background)",
        foreground: "var(--foreground)",
        card: { DEFAULT: "var(--card)", foreground: "var(--card-foreground)" },
        popover: { DEFAULT: "var(--popover)", foreground: "var(--popover-foreground)" },
        primary: { DEFAULT: "var(--primary)", foreground: "var(--primary-foreground)" },
        secondary: { DEFAULT: "var(--secondary)", foreground: "var(--secondary-foreground)" },
        muted: { DEFAULT: "var(--muted)", foreground: "var(--muted-foreground)" },
        accent: { DEFAULT: "var(--accent)", foreground: "var(--accent-foreground)" },
        destructive: { DEFAULT: "var(--destructive)", foreground: "var(--destructive-foreground)" },
        border: "var(--border)",
        input: "var(--input)",
        ring: "var(--ring)",
        sidebar: {
          DEFAULT: "var(--sidebar)",
          foreground: "var(--sidebar-foreground)",
          accent: "var(--sidebar-accent)",
          "accent-foreground": "var(--sidebar-accent-foreground)",
          border: "var(--sidebar-border)",
        },
        // CIMS 의미색 — shadcn 에 없는 것
        success: { DEFAULT: "var(--cims-success)", soft: "var(--cims-success-soft)", on: "var(--cims-success-on-soft)" },
        warning: { DEFAULT: "var(--cims-warning)", soft: "var(--cims-warning-soft)", on: "var(--cims-warning-on-soft)" },
        info: { DEFAULT: "var(--cims-info)", soft: "var(--cims-info-soft)", on: "var(--cims-info-on-soft)" },
        neutral: { DEFAULT: "var(--cims-neutral)", soft: "var(--cims-neutral-soft)", on: "var(--cims-neutral-on-soft)" },
        brandsoft: { DEFAULT: "var(--cims-brand-soft)", on: "var(--cims-brand-on-soft)" },
        dangersoft: { DEFAULT: "var(--cims-danger-soft)", on: "var(--cims-danger-on-soft)" },
        "border-strong": "var(--cims-border-strong)",
        "text-disabled": "var(--cims-text-disabled)",
      },
      borderRadius: {
        sm: "6px",
        md: "8px",
        lg: "14px",
      },
      fontSize: {
        // Figma Typography 컬렉션 그대로
        xs: ["11px", { lineHeight: "1.4" }],
        sm: ["12px", { lineHeight: "1.4" }],
        md: ["13px", { lineHeight: "1.5" }],
        base: ["14px", { lineHeight: "1.5" }],
        lg: ["16px", { lineHeight: "1.4" }],
        xl: ["18px", { lineHeight: "1.3" }],
        "2xl": ["20px", { lineHeight: "1.3" }],
        "3xl": ["24px", { lineHeight: "1.25" }],
        "4xl": ["30px", { lineHeight: "1.25" }],
      },
      fontFamily: {
        sans: ['"Pretendard Variable"', "Pretendard", "system-ui", "sans-serif"],
        mono: ['"JetBrains Mono"', "ui-monospace", "monospace"],
      },
      boxShadow: {
        sm: "var(--cims-elevation-sm)",
        lg: "var(--cims-elevation-lg)",
        focus: "var(--cims-focus-ring)",
      },
      spacing: {
        // Figma Spacing 컬렉션 (2·4·6·8·10·12·14·16·20·24·32·40·48)
        "2.5": "10px",
        "3.5": "14px",
      },
    },
  },
  plugins: [require("tailwindcss-animate")],
};

export default config;
