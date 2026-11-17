#!/usr/bin/env python3
"""Exercise actual QEMU control flow with a userspace mock, never a kext."""
import json
import os
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path,
                        default=ROOT / "qemu/build/qemu-system-x86_64",
                        help="QEMU binary to exercise (default: current development build)")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "evidence/amdsvm-noentry-mock-integration-2026-10-09.json",
                        help="JSON evidence output path")
    parser.add_argument("--case", choices=("success", "map", "types", "rom-only"),
                        help="run one mocked path instead of the full matrix")
    parser.add_argument("--ram", choices=("512M", "1G", "2G", "4G", "8G"),
                        default="512M",
                        help="guest RAM topology to exercise (default: 512M)")
    args = parser.parse_args()
    qemu_binary = args.qemu.expanduser().resolve()
    if not qemu_binary.is_file():
        parser.error(f"QEMU binary does not exist: {qemu_binary}")
    results = []
    with tempfile.TemporaryDirectory(prefix="amdsvm-noentry-") as directory:
        library = Path(directory) / "mock.dylib"
        subprocess.run(["xcrun", "clang", "-dynamiclib", "-arch", "x86_64",
                        "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tools/amdsvm_iokit_noentry_mock.c"),
                        "-framework", "IOKit", "-framework", "CoreFoundation",
                        "-o", str(library)], check=True)
        modes = (args.case,) if args.case else ("success", "map", "types", "rom-only")
        for mode in modes:
            environment = dict(os.environ)
            environment["DYLD_INSERT_LIBRARIES"] = str(library)
            environment.pop("AMDVM_MOCK_FAIL", None)
            if mode in ("map", "types"):
                environment["AMDVM_MOCK_FAIL"] = mode
            accelerator = "amdsvm,confirm-no-kdp-watchpoints=on,diagnostic-only=on"
            if mode != "rom-only":
                accelerator += ",diagnostic-validate-ram-map=on"
            result = subprocess.run([
                str(qemu_binary),
                "-L", str(ROOT / "releases/amdvm-0.1.0-rc1/pc-bios"),
                "-machine", "pc", "-accel", accelerator, "-m", args.ram,
                "-smp", "1", "-cdrom",
                str(ROOT / "artifacts/alpine/alpine-virt-3.24.2-x86_64.iso"),
                "-display", "none", "-vga", "none",
                "-serial", "none", "-monitor", "none",
                "-net", "none",
                "-no-reboot"], env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            trace = result.stdout
            selectors = list(map(int, re.findall(r"AMDVM_MOCK SELECTOR=(\d+)", trace)))
            map_matches = re.findall(
                r"AMDVM_MOCK MAP ranges=(\d+) guest_pages=(\d+)", trace)
            expected = {
                "success": [0, 3, 4, 8, 7, 11, 12, 10, 6, 0, 0],
                "map": [0, 3, 4, 8, 7, 6, 0],
                "types": [0, 3, 4, 8, 7, 11, 12, 10, 6, 0],
                "rom-only": [0, 3, 4, 8, 11, 12, 6, 0],
            }[mode]
            assert result.returncode == 0, trace
            assert selectors == expected, trace
            assert "AMDVM_MOCK END alive=0" in trace, trace
            assert "FORBIDDEN_RUN" not in trace, trace
            full_success = "RAM/NPT/MTRR pre-entry diagnostic completed" in trace
            assert full_success == (mode == "success"), trace
            if mode == "rom-only":
                assert not map_matches, trace
            else:
                assert len(map_matches) == 1, trace
                if args.ram == "2G":
                    assert tuple(map(int, map_matches[0])) == (5, 524351), trace
            results.append({"case": mode, "selectors": selectors,
                            "guest_memory_map": ([{
                                "range_count": int(ranges),
                                "guest_page_count": int(pages),
                            } for ranges, pages in map_matches]),
                            "explicit_destroy": True, "run_called": False,
                            "trace": trace})
    destination = args.output.expanduser()
    if not destination.is_absolute():
        destination = ROOT / destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps({"scope": "userspace mock; no live kernel or VMRUN",
                                       "qemu": str(qemu_binary),
                                       "qemu_sha256": sha256(qemu_binary),
                                       "test_harness_sha256": sha256(Path(__file__).resolve()),
                                       "mock_source_sha256": sha256(
                                           ROOT / "tools/amdsvm_iokit_noentry_mock.c"),
                                       "ram": args.ram,
                                       "vcpus": 1,
                                       "case_filter": args.case,
                                       "results": results}, indent=2) + "\n")
    cases_label = args.case if args.case else "four no-entry paths"
    print(f"PASS: {qemu_binary}, {cases_label}, explicit cleanup, no RUN")


if __name__ == "__main__":
    main()
