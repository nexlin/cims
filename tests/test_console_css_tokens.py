"""콘솔 소스가 쓰는 CSS 변수가 **실제로 정의돼 있는가**.

없는 변수를 쓰면 그 선언이 통째로 무효가 된다(invalid at computed-value time) — 에러도
경고도 없이 **그 스타일만 조용히 사라진다.** 실측(2026-09-17): 표의 묶음 경계선을
`var(--border-strong)` 로 그렸는데 그런 변수가 없어(정의된 이름은 `--cims-border-strong`)
선이 아예 안 그려졌다. 번들에는 코드가 들어가 있어 배포·캐시를 두 번 의심한 뒤에야 찾았다.

타입 검사도 eslint 도 이걸 못 잡는다 — 문자열이기 때문이다. 그래서 여기서 지킨다.

sys.path 조작 없음 — 소스 텍스트만 읽는다.
"""
import os
import re
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
CONSOLE = os.path.join(_REPO, "ems", "core", "console", "src")
PACKS = [os.path.join(_REPO, "ems", "service", "console", "src"),
         os.path.join(_REPO, "ems", "tester", "console", "src")]
INDEX_CSS = os.path.join(CONSOLE, "index.css")

# 런타임에 주입되는 변수 — 소스에 정의가 없는 것이 정상이다.
_EXTERNAL_PREFIXES = ('--radix-',)

_VAR_USE = re.compile(r"var\(\s*(--[A-Za-z0-9_-]+)")
_VAR_DEF = re.compile(r"(--[A-Za-z0-9_-]+)\s*:")
# 주석은 걷어낸다 — `// var(--chart-N)` 같은 설명문은 쓰는 게 아니다.
_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


def _defined() -> set:
    with open(INDEX_CSS, encoding="utf-8") as f:
        return set(_VAR_DEF.findall(f.read()))


def _used() -> dict:
    out = {}
    for root_dir in [CONSOLE] + [p for p in PACKS if os.path.isdir(p)]:
        for root, _dirs, files in os.walk(root_dir):
            if 'node_modules' in root:
                continue
            for fn in files:
                if not fn.endswith(('.ts', '.tsx')):
                    continue
                path = os.path.join(root, fn)
                with open(path, encoding="utf-8", errors="replace") as f:
                    src = _COMMENT.sub(' ', f.read())
                for name in _VAR_USE.findall(src):
                    out.setdefault(name, set()).add(os.path.relpath(path, _REPO))
    return out


def _ok(name: str, defined: set) -> bool:
    """이름이 정의됐는가. 끝이 `-` 인 것은 **템플릿 접두사**다(`var(--chart-${i})`) —
    그 접두사로 시작하는 정의가 하나라도 있으면 통과시킨다."""
    if name in defined or name.startswith(_EXTERNAL_PREFIXES):
        return True
    return name.endswith('-') and any(d.startswith(name) for d in defined)


class ConsoleCssTokens(unittest.TestCase):

    def test_쓰는_변수가_전부_정의돼_있다(self):
        defined = _defined()
        self.assertIn('--border', defined, 'index.css 를 못 읽었다 — 경로가 바뀌었나')
        missing = {k: sorted(v) for k, v in _used().items() if not _ok(k, defined)}
        self.assertEqual({}, missing,
                         '정의되지 않은 CSS 변수를 쓴다 — 그 선언은 조용히 사라진다:\n'
                         + '\n'.join(f'  {k} ← {", ".join(v)}' for k, v in missing.items()))

    def test_토큰_이름_규약(self):
        """자주 틀리는 짝 — 헷갈리면 여기 목록을 본다."""
        defined = _defined()
        self.assertIn('--cims-border-strong', defined, '표 외곽선 토큰')
        self.assertNotIn('--border-strong', defined,
                         '이름이 생겼다면 위 실측 사례(§)를 갱신하고 이 시험을 지운다')


if __name__ == '__main__':
    unittest.main(verbosity=2)
