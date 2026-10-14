#!/usr/bin/env python3
"""Exercise matrix failure handling and scratch ownership without native fixtures."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


MATRIX = Path(__file__).resolve().with_name("test_still_matrix.sh")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="matrix_harness_") as directory:
        root = Path(directory)
        work = root / "scratch"
        work.mkdir()
        sentinel = work / "unrelated"
        sentinel.write_text("keep")
        python = root / "python"
        python.write_text(f"#!{sys.executable}\n" + '''
import os
from pathlib import Path
import sys
args = sys.argv[1:]
mode = os.environ.get("MATRIX_TEST_FAIL")
if args[0] == "-c":
    print(float(args[2]) - float(args[3]))
elif len(args) == 2:
    sys.exit(1 if mode == "preflight" else 0)
elif args[1].isdigit():
    if mode == "synthesis":
        sys.exit(1)
    output = Path(args[5])
    assert len(list(output.parent.parent.iterdir())) == 1, "previous cell leaked"
    output.write_bytes(b"fixture")
else:
    assert Path(args[1]).is_file() and Path(args[2]).is_file()
''')
        python.chmod(0o755)
        codec = root / "codec"
        codec.write_text(f"#!{sys.executable}\n" + '''
import os
from pathlib import Path
import sys
Path(os.environ["MATRIX_TEST_CODEC_CALLED"]).touch()
if os.environ.get("MATRIX_TEST_FAIL") == "codec":
    sys.exit(1)
args = sys.argv[1:]
Path(args[args.index("-o") + 1]).write_bytes(b"fixture")
''')
        codec.chmod(0o755)
        called = root / "codec_called"
        env = dict(os.environ, PYTHON_BIN=str(python), GTOOLS=str(codec),
                   WORK_DIR=str(work), FAST="1", GPR_KEEP_TEST_ARTIFACTS="0",
                   MATRIX_TEST_CODEC_CALLED=str(called))
        for mode, expected in (("preflight", 2), ("synthesis", 16), ("codec", 16), ("", 0)):
            called.unlink(missing_ok=True)
            result = subprocess.run(
                ["bash", str(MATRIX)], env=dict(env, MATRIX_TEST_FAIL=mode),
                capture_output=True, text=True, timeout=30,
            )
            assert result.returncode == expected, (mode, result.stdout, result.stderr)
            assert sentinel.read_text() == "keep"
            assert list(work.iterdir()) == [sentinel], (mode, list(work.iterdir()))
            if mode in {"preflight", "synthesis"}:
                assert not called.exists(), "codec ran after failed prerequisites"
            if mode == "preflight":
                assert "raw" not in result.stdout, result.stdout
        roundtrip = MATRIX.with_name("test_gpr_tools_raw_gpr_roundtrip.sh")
        result = subprocess.run(
            ["bash", str(roundtrip)],
            env=dict(os.environ, WORK=str(work), GPR_TOOLS=str(root / "missing-codec")),
            capture_output=True, text=True, timeout=30,
        )
        assert result.returncode == 2, (result.stdout, result.stderr)
        assert sentinel.read_text() == "keep"
        assert list(work.iterdir()) == [sentinel], "roundtrip deleted caller-owned scratch"
        print("test_still_matrix_harness: PASS")


if __name__ == "__main__":
    main()
