#!/usr/bin/env python3

"""Emit the outer Meson dependency file for the nested ericw-tools build."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Iterable
import uuid


class DependencyError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--depfile", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--explicit", action="append", default=[])
    parser.add_argument("--output", action="append", required=True)
    return parser.parse_args()


def read_cmake_cache(cache_path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = cache_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise DependencyError(f"cannot read {cache_path}: {error}") from error

    for line in lines:
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        values[key] = value
    return values


def run_ninja(
    ninja: Path,
    build_dir: Path,
    arguments: list[str],
    *,
    null_terminated: bool = False,
) -> list[str]:
    try:
        result = subprocess.run(
            [str(ninja), "-C", str(build_dir), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise DependencyError(f"ninja {' '.join(arguments)} failed: {error}") from error

    if result.returncode != 0:
        details = result.stderr.decode("utf-8", errors="replace").strip()
        raise DependencyError(f"ninja {' '.join(arguments)} failed: {details}")

    if null_terminated:
        if result.stdout and not result.stdout.endswith(b"\0"):
            raise DependencyError("Ninja's NUL-delimited input list was truncated")
        raw_lines = result.stdout.split(b"\0")
        if raw_lines and raw_lines[-1] == b"":
            raw_lines.pop()
    else:
        raw_lines = result.stdout.splitlines()

    try:
        return [line.decode("utf-8") for line in raw_lines]
    except UnicodeDecodeError as error:
        raise DependencyError(f"Ninja returned a non-UTF-8 path: {error}") from error


def resolve_dependency(raw_path: str, build_dir: Path) -> Path:
    path = Path(raw_path)
    if not path.is_absolute():
        path = build_dir / path
    return Path(os.path.abspath(path))


def collect_cmake_regeneration_inputs(lines: Iterable[str], build_dir: Path) -> set[Path]:
    dependencies: set[Path] = set()
    in_inputs = False
    saw_outputs = False
    for line in lines:
        if line == "  input: RERUN_CMAKE":
            in_inputs = True
            continue
        if line.startswith("  input:"):
            raise DependencyError(f"unexpected build.ninja input rule: {line.strip()}")
        if line.startswith("  outputs:"):
            saw_outputs = True
            break
        if not in_inputs or not line.startswith("    "):
            continue
        value = line.strip()
        while value.startswith("|"):
            value = value[1:].lstrip()
        if value:
            dependencies.add(resolve_dependency(value, build_dir))
    if not in_inputs or not saw_outputs or not dependencies:
        raise DependencyError("could not parse Ninja's build.ninja regeneration inputs")
    return dependencies


def collect_valid_compiler_dependencies(
    lines: Iterable[str],
    build_dir: Path,
    expected_outputs: set[str],
) -> set[Path]:
    raw_dependencies: set[str] = set()
    records: dict[str, tuple[int, list[str]]] = {}
    current_output: str | None = None
    expected_count = 0
    current_dependencies: list[str] = []

    def finish_record() -> None:
        nonlocal current_output, expected_count, current_dependencies
        if current_output is None:
            return
        if current_output in records:
            raise DependencyError(f"Ninja returned duplicate dependency records for {current_output}")
        if len(current_dependencies) != expected_count:
            raise DependencyError(
                f"Ninja reported {expected_count} dependencies for {current_output}, "
                f"but emitted {len(current_dependencies)}"
            )
        records[current_output] = (expected_count, current_dependencies)
        current_output = None
        expected_count = 0
        current_dependencies = []

    header_pattern = re.compile(r"^(.*): #deps ([0-9]+), deps mtime .* \(([^()]*)\)$")
    for line in lines:
        if not line:
            finish_record()
            continue
        if not line.startswith((" ", "\t")):
            finish_record()
            match = header_pattern.fullmatch(line)
            if match is None:
                raise DependencyError(f"could not parse Ninja dependency record: {line}")
            current_output, count, state = match.groups()
            if state != "VALID":
                raise DependencyError(f"Ninja dependency record is not valid: {current_output} ({state})")
            expected_count = int(count)
            continue
        if current_output is None:
            raise DependencyError("Ninja emitted a dependency outside a compiler record")
        if not line.startswith("    "):
            raise DependencyError("Ninja used an unexpected compiler-dependency indentation")
        current_dependencies.append(line[4:])
    finish_record()

    actual_outputs = set(records)
    if actual_outputs != expected_outputs:
        missing = sorted(expected_outputs - actual_outputs)
        unexpected = sorted(actual_outputs - expected_outputs)
        raise DependencyError(
            f"Ninja compiler dependency records did not match the requested outputs "
            f"(missing={missing[:1]}, unexpected={unexpected[:1]})"
        )

    for _count, record_dependencies in records.values():
        raw_dependencies.update(record_dependencies)
    return {resolve_dependency(path, build_dir) for path in raw_dependencies}


def collect_ninja_dependencies(
    source_dir: Path,
    build_dir: Path,
    explicit_paths: list[str],
    output_paths: list[str],
) -> set[Path]:
    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    if cache.get("CMAKE_GENERATOR") != "Ninja":
        raise DependencyError(f"unsupported CMake generator: {cache.get('CMAKE_GENERATOR', 'unknown')}")

    ninja_value = cache.get("CMAKE_MAKE_PROGRAM")
    if not ninja_value:
        raise DependencyError("CMAKE_MAKE_PROGRAM is absent from CMakeCache.txt")
    ninja = Path(ninja_value).resolve(strict=False)
    if not ninja.is_file():
        raise DependencyError(f"cached Ninja executable is missing: {ninja}")

    dependencies = {Path(path).resolve(strict=False) for path in explicit_paths}

    outputs = [resolve_dependency(path, build_dir) for path in output_paths]
    if any(not output.is_file() for output in outputs):
        missing = [str(output) for output in outputs if not output.is_file()]
        raise DependencyError(f"required ericw output is missing: {', '.join(missing)}")
    dependencies.update(outputs)

    target_names = [Path(path).stem for path in output_paths]
    transitive_lines = run_ninja(
        ninja,
        build_dir,
        ["-t", "inputs", "-0", "-E", *target_names],
        null_terminated=True,
    )
    object_names: list[str] = []
    for line in transitive_lines:
        value = line
        if not value:
            continue
        dependency = resolve_dependency(value, build_dir)
        if value.lower().endswith((".o", ".obj", ".gch", ".pch")):
            object_names.append(value)
        if dependency != build_dir:
            dependencies.add(dependency)
    if not object_names:
        raise DependencyError("Ninja returned no object or PCH inputs for the ericw targets")

    compiler_lines = run_ninja(ninja, build_dir, ["-t", "deps", *object_names])
    dependencies.update(
        collect_valid_compiler_dependencies(compiler_lines, build_dir, set(object_names))
    )

    query_lines = run_ninja(ninja, build_dir, ["-t", "query", "build.ninja"])
    dependencies.update(collect_cmake_regeneration_inputs(query_lines, build_dir))
    dependencies.update(
        {
            (build_dir / "build.ninja").resolve(strict=False),
            (build_dir / "CMakeCache.txt").resolve(strict=False),
            (build_dir / ".viberadiant-ericw-config").resolve(strict=False),
        }
    )

    root_cmake_file = (source_dir / "CMakeLists.txt").resolve(strict=False)
    if root_cmake_file not in dependencies:
        raise DependencyError("nested Ninja graph does not belong to the requested ericw source tree")

    for cache_key in ("CMAKE_COMMAND", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM"):
        value = cache.get(cache_key)
        if value:
            dependency = Path(value).resolve(strict=False)
            if dependency.exists():
                dependencies.add(dependency)

    missing_dependencies = [path for path in dependencies if not path.exists()]
    if missing_dependencies:
        raise DependencyError(f"collected dependency is already missing: {missing_dependencies[0]}")
    return dependencies


def escape_depfile_path(path: Path | str) -> str:
    value = str(path)
    if os.name == "nt":
        value = value.replace("\\", "/")
    if any(character in value for character in ("\0", "\t", "\n", "\v", "\f", "\r")):
        raise DependencyError(f"unsupported whitespace in dependency path: {value!r}")
    if value.endswith(":"):
        raise DependencyError(f"unsupported trailing colon in dependency path: {value!r}")

    escaped: list[str] = []
    pending_backslashes = 0
    for character in value:
        if character == "\\":
            pending_backslashes += 1
            continue
        if character == " ":
            escaped.append("\\" * (pending_backslashes * 2 + 1))
            escaped.append(character)
        elif character in ("#", ":"):
            escaped.append("\\" * (pending_backslashes + 1))
            escaped.append(character)
        else:
            escaped.append("\\" * pending_backslashes)
            if character == "$":
                escaped.append("$" if pending_backslashes else "$$")
            else:
                escaped.append(character)
        pending_backslashes = 0

    # Ninja's GCC-style depfile grammar cannot represent a token ending in an
    # odd run of backslashes without consuming its following delimiter.
    if pending_backslashes % 2 != 0:
        raise DependencyError(f"unsupported trailing backslash in dependency path: {value!r}")
    escaped.append("\\" * pending_backslashes)
    return "".join(escaped)


def write_depfile_atomic(depfile: Path, target: str, dependencies: set[Path]) -> None:
    depfile.parent.mkdir(parents=True, exist_ok=True)
    escaped_target = escape_depfile_path(target)
    escaped_dependencies = " ".join(escape_depfile_path(path) for path in sorted(dependencies, key=str))
    contents = f"{escaped_target}: {escaped_dependencies}\n"

    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{depfile.name}.",
            suffix=".tmp",
            dir=depfile.parent,
            delete=False,
        ) as temporary:
            temporary_name = temporary.name
            temporary.write(contents)
        os.replace(temporary_name, depfile)
        temporary_name = None
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass


def main() -> int:
    arguments = parse_args()
    source_dir = Path(arguments.source_dir).resolve(strict=False)
    build_dir = Path(arguments.build_dir).resolve(strict=False)
    depfile = Path(arguments.depfile).resolve(strict=False)
    target = arguments.target
    resolved_target = Path(target).resolve(strict=False)

    try:
        dependencies = collect_ninja_dependencies(
            source_dir,
            build_dir,
            arguments.explicit,
            arguments.output,
        )
        dependencies.discard(depfile)
        dependencies.discard(resolved_target)
        escape_depfile_path(target)
        for dependency in dependencies:
            escape_depfile_path(dependency)
        mode = f"tracking {len(dependencies)} dependencies"
    except Exception as error:
        sentinel = depfile.with_name(f".{depfile.name}.always-check-{uuid.uuid4().hex}")
        if os.path.lexists(sentinel):
            raise DependencyError(f"could not allocate a missing fallback dependency: {sentinel}") from error
        dependencies = {sentinel}
        mode = f"conservative always-check fallback ({error})"

    write_depfile_atomic(depfile, target, dependencies)
    print(f"ericw-tools depfile: {mode}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
