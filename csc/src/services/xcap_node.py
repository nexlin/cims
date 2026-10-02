"""XCAP node selector (RFC 4825 §6.3) — 문서 안의 요소·속성 하나를 가리켜 읽고·넣고·바꾸고·지운다.

GMS 는 그룹 문서의 요소·속성 단위 절차를 지원한다(TS 24.481 §6.3.6~§6.3.12 → RFC 4825 «Create or Replace an Element» 등).
Request-URI = `<문서 URI>/~~/<node selector>[?xmlns(p=ns)…]`.

  node-selector    = step *("/" step) ["/" ("@" att-name | "namespace::*")]
  step             = NameorAny | NameorAny "[" position "]" | NameorAny "[@att=" quoted "]" | NameorAny "[" position "][@att=" quoted "]"
  NameorAny        = QName | "*"

QName 의 접두는 query 의 `xmlns(prefix=namespace)` 로 푼다(§6.4). 접두가 없는 요소 이름은 application usage 의 기본 이름공간이다.
"""
import copy
import re
import xml.etree.ElementTree as ET
from typing import List, Optional, Tuple

SEPARATOR = '/~~/'
MIME_ELEMENT = 'application/xcap-el+xml'
MIME_ATTRIBUTE = 'application/xcap-att+xml'
MIME_NAMESPACES = 'application/xcap-ns+xml'


class SelectorError(ValueError):
    """node selector 가 문법에 맞지 않는다(400)."""


class Step:
    def __init__(self, name: str, position: Optional[int], attr: Optional[Tuple[str, str]]):
        self.name, self.position, self.attr = name, position, attr     # name = '{ns}local' 또는 '*', attr = ('{ns}name' | 'name', 값)


class Selector:
    def __init__(self, steps: List[Step], attribute: Optional[str] = None, namespaces: bool = False):
        self.steps, self.attribute, self.namespaces = steps, attribute, namespaces

    @property
    def kind(self) -> str:
        return 'namespaces' if self.namespaces else ('attribute' if self.attribute else 'element')


_XMLNS = re.compile(r'xmlns\(([A-Za-z_][\w.\-]*)=([^)]*)\)')
_STEP = re.compile(r'^(\*|[A-Za-z_][\w.\-]*(?::[A-Za-z_][\w.\-]*)?)'
                   r'(?:\[(\d+)\])?'
                   r'(?:\[@([A-Za-z_][\w.\-]*(?::[A-Za-z_][\w.\-]*)?)=(?:"([^"]*)"|\'([^\']*)\')\])?$')


def parse_bindings(query: str) -> dict:
    """query 의 `xmlns(prefix=namespace)` 들(RFC 4825 §6.4) → {prefix: namespace}."""
    return {m.group(1): m.group(2) for m in _XMLNS.finditer(query or '')}


def _split_steps(text: str) -> List[str]:
    """'/' 로 가르되 따옴표 안의 '/' 는 값이다(`entry[@uri="sip:a@b/c"]`)."""
    out, cur, quote = [], '', ''
    for ch in text:
        if quote:
            cur += ch
            if ch == quote:
                quote = ''
        elif ch in '"\'':
            quote = ch
            cur += ch
        elif ch == '/':
            out.append(cur)
            cur = ''
        else:
            cur += ch
    out.append(cur)
    return out


def _qname(name: str, bindings: dict, default_ns: str, attribute: bool = False) -> str:
    if name == '*':
        return name
    if ':' in name:
        prefix, local = name.split(':', 1)
        if prefix not in bindings:
            raise SelectorError(f"namespace prefix '{prefix}' is not bound (xmlns() in the query)")
        return f'{{{bindings[prefix]}}}{local}'
    return name if attribute or not default_ns else f'{{{default_ns}}}{name}'


def parse(selector: str, bindings: dict, default_ns: str) -> Selector:
    parts = [p for p in _split_steps(selector.strip('/'))]
    if not parts or any(p == '' for p in parts):
        raise SelectorError('empty step in node selector')
    attribute, namespaces = None, False
    last = parts[-1]
    if last == 'namespace::*':
        namespaces = True
        parts = parts[:-1]
    elif last.startswith('@'):
        attribute = _qname(last[1:], bindings, default_ns, attribute=True)
        parts = parts[:-1]
    if not parts:
        raise SelectorError('node selector has no element step')
    steps = []
    for p in parts:
        m = _STEP.match(p)
        if not m:
            raise SelectorError(f"bad step '{p}'")
        name, pos, an, v1, v2 = m.groups()
        if pos is not None and int(pos) < 1:
            raise SelectorError('position starts at 1')
        attr = (_qname(an, bindings, default_ns, attribute=True), v1 if v1 is not None else (v2 or '')) if an else None
        steps.append(Step(_qname(name, bindings, default_ns), int(pos) if pos else None, attr))
    return Selector(steps, attribute, namespaces)


def _candidates(children, step: Step):
    named = [c for c in children if isinstance(c.tag, str) and (step.name == '*' or c.tag == step.name)]
    if step.position is not None:
        named = named[step.position - 1:step.position]
    if step.attr is not None:
        named = [c for c in named if c.get(step.attr[0]) == step.attr[1]]
    return named


def locate(root, sel: Selector):
    """(부모, 요소) — 요소가 없으면 (마지막 단계의 부모 또는 None, None). 한 단계가 둘 이상을 고르면 SelectorError(§6.3 — 하나만)."""
    if not sel.steps:
        return None, None
    first = _candidates([root], sel.steps[0])
    if len(first) != 1:
        return None, None
    parent, node = None, first[0]
    for step in sel.steps[1:]:
        found = _candidates(list(node), step)
        if len(found) > 1:
            raise SelectorError('node selector matches more than one element')
        if not found:
            return node, None
        parent, node = node, found[0]
    return parent, node


def parent_exists(root, sel: Selector) -> Optional[ET.Element]:
    """마지막 단계의 부모 요소 — 없으면 None(PUT 은 409 no-parent)."""
    if len(sel.steps) < 2:
        return None
    parent_sel = Selector(sel.steps[:-1])
    _p, node = locate(root, parent_sel)
    return node


def put_element(root, sel: Selector, new_el) -> Tuple[bool, ET.Element]:
    """요소를 바꾸거나 넣는다(RFC 4825 §8.2.3). 반환 = (새로 만들었나, 새 문서 루트). 실패는 SelectorError 가 아니라 ValueError
    ('no-parent' | 'cannot-insert')."""
    root = copy.deepcopy(root)
    parent, node = locate(root, sel)
    last = sel.steps[-1]
    if node is not None:
        if parent is None:
            raise ValueError('cannot-insert')                 # 문서 루트 교체는 문서 PUT 으로 한다
        idx = list(parent).index(node)
        parent.remove(node)
        parent.insert(idx, new_el)
        created = False
    else:
        par = parent_exists(root, sel)
        if par is None:
            raise ValueError('no-parent')
        # 넣는 자리 — 같은 이름의 마지막 형제 뒤, 없으면 부모의 끝(§8.2.3)
        same = [i for i, c in enumerate(list(par)) if isinstance(c.tag, str) and (last.name == '*' or c.tag == last.name)]
        par.insert(same[-1] + 1 if same else len(list(par)), new_el)
        created = True
    # 넣은 뒤 같은 selector 가 그 요소를 골라야 한다(§8.2.3 — 아니면 cannot-insert)
    try:
        _p, check = locate(root, sel)
    except SelectorError:
        check = None
    if check is not new_el:
        raise ValueError('cannot-insert')
    return created, root


def delete_element(root, sel: Selector) -> Optional[ET.Element]:
    """요소를 지운 새 문서 루트 — 요소가 없으면 None(404)."""
    root = copy.deepcopy(root)
    parent, node = locate(root, sel)
    if node is None or parent is None:
        return None
    parent.remove(node)
    return root
