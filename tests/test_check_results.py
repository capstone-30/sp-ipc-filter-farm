"""제출 검사 회귀용 합성 fixture이며 실제 측정 자료가 아니다."""

import hashlib
import itertools
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BACKENDS = ("pipe", "sysv_mq", "sysv_shm")
SIZES = (64, 80, 96, 104, 256, 512, 1024, 2048)


def write_csv(path, cases):
    rows = [",".join(f"column{i}" for i in range(1, 26))]
    for backend, size, clients, filter_name, depth, mode in cases:
        row = [backend, path.stem, size, clients, filter_name, depth] + [1] * 19
        row[22] = mode
        rows.append(",".join(map(str, row)))
    path.write_text("\n".join(rows) + "\n")


def seal(directory):
    paths = sorted(p for p in directory.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
    (directory / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(directory)}\n"
        for p in paths
    ))


with tempfile.TemporaryDirectory(prefix="ipcff-results-test-") as work:
    basic, app, improvement = (Path(work) / phase for phase in ("basic", "app", "improve"))
    for directory in (basic, app, improvement):
        (directory / "graphs").mkdir(parents=True)
        (directory / "environment.txt").write_text(f"phase={directory.name} fixture=yes\n")
        (directory / "median.csv").write_text("synthetic fixture\n")

    basic_sizes = (64, 1024, 16384, 262144, 1048576)
    write_csv(basic / "basic.csv", (
        (b, s, c, "gray", 1, m) for b, s, c, m in
        itertools.product(BACKENDS, basic_sizes, (1, 2, 4, 8), ("latency", "throughput"))
    ))
    s1_cases = [(b, s, 1, "gray", 1, "app") for b, s in itertools.product(BACKENDS, SIZES)]
    write_csv(app / "s1.csv", s1_cases)
    write_csv(app / "s2.csv", ((b, 512, c, "gray", 1, "app") for b, c in itertools.product(BACKENDS, (1, 2, 4))))
    write_csv(app / "s3.csv", ((b, 512, 2, f, 1, "app") for b, f in itertools.product(BACKENDS, ("gray", "blur", "edge"))))
    write_csv(app / "s4.csv", ((b, 512, 1, "gray", d, "app") for b, d in itertools.product(BACKENDS, (1, 4, 16))))
    write_csv(app / "s5.csv", [("sysv_mq", 1024, 1, "gray", 1, "default")])
    write_csv(improvement / "improvement.csv", (
        (b, 512, 2, "gray", 4, m) for b, m in itertools.product(BACKENDS, ("baseline", "inplace"))
    ))
    for name in ("payload-latency", "payload-throughput", "payload-messages"):
        (basic / "graphs" / f"basic-{name}.png").write_bytes(b"fixture")
    for size, metric in itertools.product(basic_sizes, ("latency", "throughput")):
        (basic / "graphs" / f"basic-clients-{size}-{metric}.png").write_bytes(b"fixture")
    for name in ("payload-latency", "payload-throughput", "clients-throughput", "cpu"):
        (app / "graphs" / f"app-{name}.png").write_bytes(b"fixture")
    (improvement / "graphs" / "improvement.png").write_bytes(b"fixture")
    for directory in (basic, app, improvement):
        seal(directory)

    def check(expected_success):
        result = subprocess.run(
            ["bash", str(ROOT / "bench/check_results.sh"), str(basic), str(app), str(improvement)],
            capture_output=True, text=True,
        )
        assert (result.returncode == 0) == expected_success, result.stdout + result.stderr

    check(True)
    # 해시를 갱신해 파일 손상이 아닌 실험 조건 누락으로 거절되는지 확인한다.
    for backend, size in itertools.product(BACKENDS, SIZES[:4]):
        write_csv(app / "s1.csv", (case for case in s1_cases if case[:2] != (backend, size)))
        seal(app)
        check(False)
    write_csv(app / "s1.csv", (case for case in s1_cases if case[1] >= 256))
    seal(app)
    check(False)
    print("check_results: 전체 S1 통과·작은 크기별/IPC별 누락 12종·기존 범위 거절 OK")
