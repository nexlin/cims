#!/usr/bin/env python3
"""
CIMS 전체 검증 실행 및 리포트 생성
Usage: python3 run_all.py
"""
import sys, os, time
from datetime import datetime

sys.path.insert(0, os.path.dirname(__file__))

from test_csc import run_csc_tests
from test_cmp import run_cmp_tests
from test_csp import run_csp_tests
from test_e2e import run_e2e_tests
from test_volte_service import run_volte_tests
from test_ptt_service import run_ptt_tests
from test_media import run_media_tests
from test_sip_runtime import run_sip_runtime_tests
from clean_env import clean_and_restart


def generate_report(modules, elapsed):
    """Markdown 리포트 생성"""
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    total_all = sum(m["total"] for m in modules)
    pass_all = sum(m["pass"] for m in modules)
    fail_all = sum(m["fail"] for m in modules)
    skip_all = sum(m["skip"] for m in modules)
    rate = (pass_all / total_all * 100) if total_all > 0 else 0

    lines = []
    lines.append("# CIMS 시스템 검증 결과 리포트")
    lines.append("")
    lines.append(f"**검증 일시:** {now}")
    lines.append(f"**소요 시간:** {elapsed:.1f}초")
    lines.append(f"**검증 환경:** 127.0.0.1 (CSC:4420, CSP:5060/4421, CMP:9000)")
    lines.append("")
    lines.append("---")
    lines.append("")
    lines.append("## 종합 결과")
    lines.append("")
    lines.append(f"| 항목 | 값 |")
    lines.append(f"|------|----|")
    lines.append(f"| 총 검증 항목 | {total_all}건 |")
    lines.append(f"| 성공 (PASS) | {pass_all}건 |")
    lines.append(f"| 실패 (FAIL) | {fail_all}건 |")
    lines.append(f"| 건너뜀 (SKIP) | {skip_all}건 |")
    lines.append(f"| **합격률** | **{rate:.1f}%** |")
    lines.append("")

    # 모듈별 요약
    lines.append("## 모듈별 요약")
    lines.append("")
    lines.append("| 모듈 | 전체 | PASS | FAIL | SKIP | 합격률 |")
    lines.append("|------|------|------|------|------|--------|")
    for m in modules:
        mr = (m["pass"] / m["total"] * 100) if m["total"] > 0 else 0
        lines.append(f"| {m['module']} | {m['total']} | {m['pass']} | {m['fail']} | {m['skip']} | {mr:.0f}% |")
    lines.append("")

    # 상세 결과
    for m in modules:
        lines.append(f"## {m['module']} 모듈 상세")
        lines.append("")
        lines.append("| ID | 항목 | 결과 | 소요(ms) | 상세 |")
        lines.append("|----|------|------|----------|------|")
        for r in m["results"]:
            icon = {"PASS": "PASS", "FAIL": "**FAIL**", "SKIP": "SKIP"}[r["status"]]
            detail = r["detail"][:80].replace("|", "/").replace("\n", " ") if r["detail"] else ""
            lines.append(f"| {r['id']} | {r['name']} | {icon} | {r['elapsed_ms']} | {detail} |")
        lines.append("")

    # 실패 항목 요약
    failures = []
    for m in modules:
        for r in m["results"]:
            if r["status"] == "FAIL":
                failures.append(r)

    if failures:
        lines.append("## 실패 항목 분석")
        lines.append("")
        for f in failures:
            lines.append(f"### {f['id']} — {f['name']}")
            lines.append(f"- **상세:** {f['detail']}")
            lines.append("")

    lines.append("---")
    lines.append(f"*리포트 생성: {now}*")

    return "\n".join(lines)


def main():
    print("=" * 60)
    print("  CIMS 시스템 검증 시작")
    print("=" * 60)

    # 환경 초기화: 서비스 종료 → DB/로그 정리 → 서비스 재시작
    clean_and_restart()

    t0 = time.time()
    modules = []

    # 1. CMP (의존성 없음, 먼저 실행)
    print("\n" + "=" * 60)
    print("  [1/8] CMP 모듈 검증")
    print("=" * 60)
    modules.append(run_cmp_tests())

    # 2. CSP (CMP 의존)
    print("\n" + "=" * 60)
    print("  [2/8] CSP 모듈 검증")
    print("=" * 60)
    modules.append(run_csp_tests())

    # 3. CSC (독립)
    print("\n" + "=" * 60)
    print("  [3/8] CSC 모듈 검증")
    print("=" * 60)
    modules.append(run_csc_tests())

    # 4. E2E (전체 연동)
    print("\n" + "=" * 60)
    print("  [4/8] 연동/E2E 검증")
    print("=" * 60)
    modules.append(run_e2e_tests())

    # 5. VoLTE 서비스 검증
    print("\n" + "=" * 60)
    print("  [5/8] VoLTE 서비스 검증")
    print("=" * 60)
    modules.append(run_volte_tests())

    # 6. PTT 서비스 검증
    print("\n" + "=" * 60)
    print("  [6/8] PTT 서비스 검증")
    print("=" * 60)
    modules.append(run_ptt_tests())

    # 7. 미디어/녹취 검증
    print("\n" + "=" * 60)
    print("  [7/8] 미디어/녹취 검증")
    print("=" * 60)
    modules.append(run_media_tests())

    # 8. SIP 런타임 설정 (P1~P6)
    print("\n" + "=" * 60)
    print("  [8/8] SIP 런타임 설정 검증 (P1~P6)")
    print("=" * 60)
    modules.append(run_sip_runtime_tests())

    elapsed = time.time() - t0

    # 결과 출력
    print("\n" + "=" * 60)
    print("  검증 완료 — 종합 결과")
    print("=" * 60)

    total_all = sum(m["total"] for m in modules)
    pass_all = sum(m["pass"] for m in modules)
    fail_all = sum(m["fail"] for m in modules)
    rate = (pass_all / total_all * 100) if total_all > 0 else 0

    for m in modules:
        mr = (m["pass"] / m["total"] * 100) if m["total"] > 0 else 0
        icon = "\033[32m" if m["fail"] == 0 else "\033[31m"
        print(f"  {icon}{m['module']:8s}\033[0m  {m['pass']}/{m['total']} ({mr:.0f}%)")

    print(f"\n  \033[1m총 {total_all}건: PASS={pass_all} FAIL={fail_all} 합격률={rate:.1f}%\033[0m")
    print(f"  소요 시간: {elapsed:.1f}초")

    # 리포트 저장
    report = generate_report(modules, elapsed)
    report_path = os.path.join(os.path.dirname(__file__), "verification_report.md")
    with open(report_path, "w", encoding="utf-8") as f:
        f.write(report)
    print(f"\n  리포트 저장: {report_path}")

    return 0 if fail_all == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
