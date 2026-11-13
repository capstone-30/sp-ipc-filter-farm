"""패키징 회귀용 임시 프로젝트다. 실제 제출·측정 자료를 변경하지 않는다."""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="ipcff-package-test-") as work:
    root = Path(work)
    for directory in ("src", "tests", "bench/results/unselected", "docs/04-test",
                      ".github/scripts", "submission", "tmp"):
        (root / directory).mkdir(parents=True)
    shutil.copyfile(ROOT / "bench/package.sh", root / "bench/package.sh")
    for name in ("Makefile", "README.md", "AGENTS.md", "src/server.c",
                 "tests/test_basic_options.c", "tests/test_basic_options",
                 "bench/results/unselected/large.bin", ".github/scripts/check_docs.py",
                 "docs/04-test/02-server-environment.md", "submission/presentation-draft.pptx"):
        (root / name).write_text(name)
    # 결과 검사의 동작은 test_check_results.py가 검증한다.
    (root / "bench/check_results.sh").write_text("exit 0\n")
    env = {**os.environ, "TEAM": "99", "TMPDIR": str(root / "tmp")}
    for variable, name, csvs in (
        ("BASIC_RESULTS", "basic", ["basic"]),
        ("APP_RESULTS", "app", ["s1", "s2", "s3", "s4", "s5"]),
        ("IMPROVEMENT_RESULTS", "improve", ["improvement"]),
    ):
        directory = root / name
        (directory / "graphs").mkdir(parents=True)
        for csv in [*csvs, "median"]:
            (directory / f"{csv}.csv").write_text("synthetic fixture\n")
        env[variable] = str(directory)
    for variable, name in (("FINAL_REPORT", "report.pdf"),
                           ("PRESENTATION", "slides.pptx"), ("VIDEO", "video.mp4")):
        path = root / name
        path.write_text(name)
        env[variable] = str(path)

    def run(*args, success=True):
        result = subprocess.run(["bash", str(root / "bench/package.sh"), *args],
                                env=env, capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        assert not list((root / "tmp").iterdir()), "staging directory leaked"

    for suffix in ("-draft", ""):
        run(*(["--draft"] if suffix else []))
        archive = root / f"submission/osproject_99{suffix}.tar"
        with tarfile.open(archive) as tar:
            names = set(tar.getnames())
            assert "tests/test_basic_options.c" in names
            assert ".github/scripts/check_docs.py" in names
            assert "tests/test_basic_options" not in names
            assert not any(name.startswith("bench/results") for name in names)
            if not suffix:
                for name, original in (("report.pdf", "report.pdf"),
                                       ("presentation.pptx", "slides.pptx"), ("video.mp4", "video.mp4")):
                    assert tar.extractfile(f"submission/{name}").read() == original.encode()
                assert "evidence/basic/basic.csv" in names
            else:
                assert not any(name.startswith("evidence/") for name in names)
        before = archive.read_bytes()
        run(*(["--draft"] if suffix else []), success=False)
        assert archive.read_bytes() == before
    # 복사 실패도 임시 디렉터리를 회수하고 불완전한 tar를 게시하지 않는다.
    env["TEAM"] = "98"
    (root / "src").rename(root / "missing-src")
    run("--draft", success=False)
    assert not (root / "submission/osproject_98-draft.tar").exists()
print("package: final artifacts, selected evidence, binary exclusion, cleanup, overwrite protection OK")
