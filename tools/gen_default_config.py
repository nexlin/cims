#!/usr/bin/env python3
"""config_template.json 의 sections[].fields[].default 로 base <module>.json 생성.

`make dist` 단계에서 호출된다. configure 를 거치지 않는 상용 흐름
(build → pkg → 업로드 → 설치 → web 설정 → start)에서도 tarball 에 모듈이
기동 시 읽는 base conf(csp.json/cmp.json 등)가 항상 포함되도록 보장한다.
(configure 미실행 시 `bin/<m> config/<m>.json` 이 파일을 못 읽어 start 실패하던
gap 보완.)

configure.sh 의 apply_config_template 과 **동일한 set_path 구조**를 쓰되,
deploy_value(@VAR@ placeholder) 대신 중립 `default` 값을 사용한다. 운영값
(@CMP_IP@/@DB_HOST@ 등)은 설치 후 web overlay(config.json)가 덮어쓴다.

usage: gen_default_config.py <config_template.json> <out.json>
"""
import json
import os
import sys


def set_path(root, dotted, value):
    cur = root
    keys = dotted.split('.')
    for k in keys[:-1]:
        cur = cur.setdefault(k, {})
    cur[keys[-1]] = value


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: gen_default_config.py <config_template.json> <out.json>\n")
        return 2
    src, dst = sys.argv[1], sys.argv[2]

    # 파일이 이미 있으면 생성하지 않는다(non-clobber). configure.sh(env 값) 또는
    # web overlay 가 만든 conf 를 재빌드 시 기본값으로 되돌리지 않기 위함. 본
    # 생성기는 "부재 시 기본값으로 채워 start 가능하게" 하는 gap 보완 전용.
    if os.path.isfile(dst):
        sys.stderr.write(f"gen_default_config: keep existing {dst}\n")
        return 0

    with open(src, encoding='utf-8') as f:
        tpl = json.load(f)

    out = {}
    for section in tpl.get('sections', []):
        for field in section.get('fields', []):
            key = field.get('key')
            if not key:
                continue
            set_path(out, key, field.get('default'))

    with open(dst, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=4, ensure_ascii=False)
    return 0


if __name__ == '__main__':
    sys.exit(main())
