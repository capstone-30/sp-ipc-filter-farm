# IEEE 논문 템플릿

`IEEEtran`(`conference` 옵션) 기반의 IEEE 학회 논문용 LaTeX 템플릿이다.

## 구성 파일

| 파일 | 역할 |
| --- | --- |
| [main.tex](main.tex) | 본문 템플릿 (`\documentclass[conference]{IEEEtran}`) |
| [references.bib](references.bib) | IEEEtran 스타일용 BibTeX 참고문헌 예제 |
| [Makefile](Makefile) | 로컬 `pdflatex` + `bibtex` 빌드 및 정리 스크립트 |

## 사용 방법

- **Overleaf**: 이 디렉터리의 `main.tex`와 `references.bib`를 업로드해 바로 컴파일한다.
- **로컬 환경**: TeX Live(`texlive-publishers` 등 `IEEEtran.cls` 포함 패키지)를 설치한 뒤 아래 명령을 실행한다.

```bash
cd docs/paper
make
```

현재 저장소 작업 환경에는 `pdflatex`가 설치되어 있지 않아 로컬 PDF 빌드는 실행하지 않았다.
