#!/usr/bin/env python3
"""문서 표준 검사·동기화 도구.

원본은 Projects/doc-standard/check_docs.py 하나다. 각 저장소의
.github/scripts/check_docs.py는 `--sync`로 복사한 사본이다.

  python3 .github/scripts/check_docs.py [저장소 경로]   # 검사(CI)
  python3 doc-standard/check_docs.py --sync <저장소...>  # 공통 구간·사본·워크플로 동기화
"""
import os
import re
import shutil
import sys
import unicodedata
from urllib.parse import unquote

START = "<!-- doc-standard:start — doc-standard에서 동기화한다. 이 구간은 직접 고치지 않는다. -->"
END = "<!-- doc-standard:end -->"
MARK_RE = re.compile(r"<!-- doc-standard:start[^\n]*-->\n.*?<!-- doc-standard:end -->", re.S)

AGENTS_BLOCK = """\
- 작업 전 [요구사항]({FUNC_ENTRY}) → [기술 구조]({D}03-tech/01-architecture.md) 순서로 읽는다. 충돌 시 요구사항을 우선하고, 변경 사항은 구현 전에 문서에 반영한다.
- 문서는 필수 정보만 원본 한 곳에 짧게 적고 중복은 링크로 대신한다. 코드·설정의 상세 목록과 완료 이력은 복제하지 않는다.
- 현재 구현, 목표, 검증 결과를 구분한다. 미확정 사항은 대기 이유와 재개 조건을 적고, 실행한 검사와 미확인 항목을 보고한다.
- README 순서는 소개 → 기술 스택 → 시작하기 → 사용 방법 → 테스트 → 관련 문서다(해당 없으면 생략).
- 문장은 주어와 서술어를 갖춰 쓰고, 명사 나열 대신 목록이나 표로 나눈다.

## 문서 관리

- 위치는 [분류 기준](README.md#문서-분류-기준)을 따른다. 새 명세는 분류 목차 `README.md`에, 비명세 자료는 [관련 문서](README.md#관련-문서)에 등록한다.
- 명세 본문은 분류마다 `01-...md`부터 빈 번호 없이 매긴다(`api.md` 등 고정 이름 제외). 본문이 1개뿐이면 `README.md`를 생략하고, 2개 이상일 때만 목차 전용 `README.md`를 둔다.
- `01-architecture.md`는 `아키텍처와 경계` → `소스 구조` → (고유 기술 절) → `남은 작업과 리스크` 순서로 구성한다.
- 테스트 판정은 최신 결과만 남기고 변경 이력은 로그·PR에 둔다. 지침은 `AGENTS.md`, 실행 안내는 루트 `README.md`에서 관리한다.
- 문서 이동·제목 변경 후 `python3 .github/scripts/check_docs.py`로 링크·앵커·등록 여부를 확인한다(CI 동일)."""

CATEGORIES = (
    ("01-func", "범위, 역할별 기능, 업무 정책, 완료 기준", True),
    ("02-uxui", "화면 배치·조작·디자인 기준·시각 자산", False),
    ("03-tech", "아키텍처, 소스 구조, 데이터 모델, REST/IPC 계약", True),
    ("04-test", "검증 방법, 판정 기준, 최신 판정과 증거", True),
    ("05-ops", "배포·인프라 구성·운영 점검·장애 대응", False),
)

README_BLOCK = """\
| 위치 | 담는 내용 |
| --- | --- |
{CAT_ROWS}
| {ROOT} 루트·주제 폴더 | 비명세 자료(과제 안내, 보고서, 발표, 협업 규칙, 자료 수집 기록) |
| 코드 저장소 루트 README | 로컬 설치·실행·테스트 명령 |

API 상세 원본은 OpenAPI·라우터·DTO이며 명세에는 원본 위치와 한계만 적는다. 배포 절차는 `05-ops/`(없으면 기술 명세)에 두고 로컬 실행 명령은 README를 링크한다."""

WORKFLOW = """\
name: docs

on:
  pull_request:
    paths: ['**.md', '.github/scripts/check_docs.py', '.github/workflows/docs.yml']
  push:
    branches: [main]
    paths: ['**.md', '.github/scripts/check_docs.py', '.github/workflows/docs.yml']

permissions:
  contents: read

jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: '3.12'
      - run: python3 .github/scripts/check_docs.py
"""

SKIP_DIRS = {".git", "node_modules", ".venv", "venv", ".runtime", ".pytest_cache", "build",
             ".gradle", ".next", "dist", "target", "site-packages", "__pycache__", ".idea"}
LINK_RE = re.compile(r"!?\[(?:[^\]\[]|\[[^\]]*\])*\]\(<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\)")
NAV_RE = re.compile(r"^\[[^\]]+\]\([^)#][^)]*\)(\s*·\s*\[[^\]]+\]\([^)]+\))+\s*$")
NUM_RE = re.compile(r"^(\d{2})-.+\.md$")


def docs_prefix(root):
    if os.path.isdir(os.path.join(root, "docs", "01-func")):
        return "docs/"
    if os.path.isdir(os.path.join(root, "01-func")):
        return ""
    return None


def category_entry(docs_dir, d, cat):
    cdir = os.path.join(docs_dir, cat)
    if not os.path.isdir(cdir):
        return f"{d}{cat}/README.md"
    files = sorted(x for x in os.listdir(cdir) if x.endswith(".md") and x != "README.md")
    if len(files) == 1:
        return f"{d}{cat}/{files[0]}"
    return f"{d}{cat}/README.md"


def render(tpl, root, d):
    docs_dir = os.path.join(root, d) if d else root
    rows = []
    for cat, desc, req in CATEGORIES:
        if req or os.path.isdir(os.path.join(docs_dir, cat)):
            rows.append(f"| [`{d}{cat}/`]({category_entry(docs_dir, d, cat)}) | {desc} |")
    return (tpl
            .replace("{D}", d)
            .replace("{ROOT}", "`docs/`" if d else "이 저장소")
            .replace("{FUNC_ENTRY}", category_entry(docs_dir, d, "01-func"))
            .replace("{CAT_ROWS}", "\n".join(rows)))


def block(body):
    return f"{START}\n{body}\n{END}"


def md_files(root):
    for dp, dn, fn in os.walk(root):
        dn[:] = sorted(x for x in dn if x not in SKIP_DIRS)
        for f in sorted(fn):
            if f.endswith(".md"):
                yield os.path.join(dp, f)


def slug(text):
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text.strip()).lower()
    text = re.sub(r"<[^>]+>", "", text)
    out = []
    for ch in text:
        if ch == " ":
            out.append("-")
        elif ch in "-_" or unicodedata.category(ch)[0] in "LNM":
            out.append(ch)
    return "".join(out)


def body_lines(path):
    """코드 블록을 제외한 (줄 번호, 줄) 목록."""
    fence = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for i, line in enumerate(fh, 1):
            s = line.rstrip("\r\n")
            m = re.match(r"^\s*(```|~~~)", s)
            if m:
                fence = None if fence == m.group(1) else (fence or m.group(1))
                continue
            if fence is None:
                yield i, s


_anchor_cache = {}


def anchors(path):
    if path not in _anchor_cache:
        res, seen = set(), {}
        for _, s in body_lines(path):
            m = re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", s)
            if m:
                base = slug(m.group(1))
                n = seen.get(base, 0)
                seen[base] = n + 1
                res.add(base if n == 0 else f"{base}-{n}")
            for a in re.findall(r"<a\s+(?:name|id)=\"([^\"]+)\"", s):
                res.add(a.lower())
        _anchor_cache[path] = res
    return _anchor_cache[path]


def check(root):
    root = os.path.abspath(root)
    d = docs_prefix(root)
    errors = []
    rel = lambda p: os.path.relpath(p, root)
    if d is None:
        return [f"{root}: 01-func 폴더가 없다(docs/01-func 또는 01-func)"]
    docs_dir = os.path.join(root, d) if d else root

    # 1. 분류 폴더 구조·단일 문서 README 금지·다중 문서 목차 전용 검사
    if not os.path.isfile(os.path.join(docs_dir, "03-tech", "01-architecture.md")):
        errors.append(f"필수 문서 없음: {d}03-tech/01-architecture.md")
    for cat, _, req in CATEGORIES:
        cdir = os.path.join(docs_dir, cat)
        if not os.path.isdir(cdir):
            if req:
                errors.append(f"필수 폴더 없음: {d}{cat}")
            continue
        all_md = sorted(x for x in os.listdir(cdir) if x.endswith(".md"))
        content_md = [x for x in all_md if x != "README.md"]
        has_readme = "README.md" in all_md
        if not content_md:
            errors.append(f"{d}{cat}: 본문 문서(01-...md)가 없다")
        elif len(content_md) == 1 and has_readme:
            errors.append(f"{d}{cat}: 단일 문서 폴더({content_md[0]})에는 README.md를 두지 않는다")
        elif len(content_md) >= 2 and not has_readme:
            errors.append(f"{d}{cat}: 본문 문서가 2개 이상이면 목차 README.md가 필요하다")
        elif len(content_md) >= 2 and has_readme:
            readme_p = os.path.join(cdir, "README.md")
            h2 = [s for _, s in body_lines(readme_p) if s.startswith("## ")]
            if h2:
                errors.append(f"{d}{cat}/README.md: 다중 문서 폴더의 README.md는 목차만 둔다(본문 절 {h2} 발견)")
        nums = sorted(int(m.group(1)) for x in content_md if (m := NUM_RE.match(x)))
        if not nums or nums != list(range(1, len(nums) + 1)):
            errors.append(f"{d}{cat}: 번호가 01부터 연속이 아니다 {nums}")

    # 2. 링크·앵커, 역참조 수집
    linked = set()
    for f in md_files(root):
        for i, s in body_lines(f):
            s = re.sub(r"`[^`]*`", "", s)
            for target in LINK_RE.findall(s):
                if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I):
                    continue
                path, _, frag = target.partition("#")
                dest = os.path.normpath(os.path.join(os.path.dirname(f), unquote(path))) if path else f
                if path:
                    linked.add(dest)
                if not os.path.exists(dest):
                    errors.append(f"{rel(f)}:{i}: 없는 대상 {target}")
                elif frag and dest.endswith(".md") and unquote(frag).lower() not in anchors(dest):
                    errors.append(f"{rel(f)}:{i}: 없는 앵커 {target}")

    # 3. 등록되지 않은 문서
    for f in md_files(docs_dir):
        r = rel(f)
        if r.startswith(".github") or "/.github/" in f or os.path.basename(f) == "AGENTS.md":
            continue
        if f == os.path.join(root, "README.md"):
            continue
        if f not in linked:
            errors.append(f"{r}: 어느 문서에서도 링크하지 않는다(분류 목차나 관련 문서에 등록)")

    # 4. 기술 구조 절 순서
    arch = os.path.join(docs_dir, "03-tech", "01-architecture.md")
    if os.path.isfile(arch):
        h2 = [s[3:].strip() for _, s in body_lines(arch) if s.startswith("## ")]
        head = ["아키텍처와 경계", "소스 구조"]
        tail = "남은 작업과 리스크"
        for need in [*head, tail]:
            if need not in h2:
                errors.append(f"{rel(arch)}: '## {need}' 절이 없다")
        if h2[:2] != head or (h2 and h2[-1] != tail):
            errors.append(
                f"{rel(arch)}: 절 순서는 아키텍처와 경계 → 소스 구조 → (고유 기술 절) → 남은 작업과 리스크이다 (현재 {h2})"
            )

    # 5. 하단 탐색 줄 뒤의 제목
    for f in md_files(docs_dir):
        nav, seen_h2 = None, False
        for i, s in body_lines(f):
            if s.startswith("## "):
                seen_h2 = True
            if seen_h2 and NAV_RE.match(s):
                nav = i
            elif nav and re.match(r"^#{1,6}\s", s):
                errors.append(f"{rel(f)}:{i}: 하단 탐색 줄({nav}행) 뒤에 절이 있다")
                break

    # 6. 공통 구간
    for name, tpl in (("AGENTS.md", AGENTS_BLOCK), ("README.md", README_BLOCK)):
        p = os.path.join(root, name)
        if not os.path.isfile(p):
            errors.append(f"{name} 없음")
            continue
        text = open(p, encoding="utf-8").read().replace("\r\n", "\n")
        m = MARK_RE.search(text)
        if not m:
            errors.append(f"{name}: doc-standard 공통 구간이 없다")
        elif m.group(0) != block(render(tpl, root, d)):
            errors.append(f"{name}: 공통 구간이 원본과 다르다(doc-standard에서 --sync)")
    return errors


def sync(root):
    root = os.path.abspath(root)
    d = docs_prefix(root)
    if d is None:
        print(f"건너뜀 {root}: 01-func 없음")
        return
    for name, tpl in (("AGENTS.md", AGENTS_BLOCK), ("README.md", README_BLOCK)):
        p = os.path.join(root, name)
        raw = open(p, encoding="utf-8").read()
        crlf = "\r\n" in raw
        text = raw.replace("\r\n", "\n")
        if not MARK_RE.search(text):
            sys.exit(f"{p}: 공통 구간 표식이 없다. 표식을 먼저 넣는다.")
        new = MARK_RE.sub(lambda _: block(render(tpl, root, d)), text)
        with open(p, "w", encoding="utf-8", newline="") as fh:
            fh.write(new.replace("\n", "\r\n") if crlf else new)
    scripts = os.path.join(root, ".github", "scripts")
    os.makedirs(scripts, exist_ok=True)
    if os.path.abspath(__file__) != os.path.join(scripts, "check_docs.py"):
        shutil.copyfile(__file__, os.path.join(scripts, "check_docs.py"))
    wf = os.path.join(root, ".github", "workflows")
    os.makedirs(wf, exist_ok=True)
    with open(os.path.join(wf, "docs.yml"), "w", encoding="utf-8") as fh:
        fh.write(WORKFLOW)
    print(f"동기화 {root}")


def main(argv):
    if argv and argv[0] == "--sync":
        for r in argv[1:]:
            sync(r)
        return 0
    roots = argv or ["."]
    failed = 0
    for r in roots:
        errs = check(r)
        label = os.path.abspath(r)
        print(f"{'FAIL' if errs else 'OK'} {label} ({len(errs)}건)")
        for e in errs:
            print(f"  - {e}")
        failed += bool(errs)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
