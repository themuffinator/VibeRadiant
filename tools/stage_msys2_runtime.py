#!/usr/bin/env python3
"""Stage the transitive MSYS2 runtime closure for a Windows installation."""

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path, PurePosixPath


SCHEMA_VERSION = 1
MANIFEST_NAME = ".viberadiant-msys2-runtime.json"
LOCK_NAME = MANIFEST_NAME + ".lock"
READ_SIZE = 1024 * 1024
COMMON_PLUGIN_GROUPS = (
	"imageformats",
	"platforms",
	"styles",
	"iconengines",
	"tls",
	"ssl",
	"multimedia",
)
QT5_PLUGIN_GROUPS = ("audio", "mediaservice", "playlistformats")
API_SET_PREFIXES = ("api-ms-win-", "ext-ms-win-")


class PEFormatError(RuntimeError):
	pass


def read_exact(source, offset, length, path):
	source.seek(offset)
	data = source.read(length)
	if len(data) != length:
		raise PEFormatError("truncated PE data in {}".format(path))
	return data


def read_pe_imports(path):
	with path.open("rb") as source:
		dos_header = read_exact(source, 0, 64, path)
		if dos_header[:2] != b"MZ":
			raise PEFormatError("not a PE executable: {}".format(path))
		pe_offset = struct.unpack_from("<I", dos_header, 0x3C)[0]
		pe_header = read_exact(source, pe_offset, 24, path)
		if pe_header[:4] != b"PE\0\0":
			raise PEFormatError("invalid PE signature: {}".format(path))

		machine, section_count = struct.unpack_from("<HH", pe_header, 4)
		optional_size = struct.unpack_from("<H", pe_header, 20)[0]
		optional = read_exact(source, pe_offset + 24, optional_size, path)
		magic = struct.unpack_from("<H", optional, 0)[0]
		if magic == 0x10B:
			image_base = struct.unpack_from("<I", optional, 28)[0]
			directory_count_offset = 92
			directory_offset = 96
		elif magic == 0x20B:
			image_base = struct.unpack_from("<Q", optional, 24)[0]
			directory_count_offset = 108
			directory_offset = 112
		else:
			raise PEFormatError("unsupported PE optional-header magic in {}".format(path))
		if optional_size < directory_count_offset + 4:
			raise PEFormatError("truncated PE optional header in {}".format(path))

		directory_count = struct.unpack_from("<I", optional, directory_count_offset)[0]
		size_of_headers = struct.unpack_from("<I", optional, 60)[0]
		section_data = read_exact(source, pe_offset + 24 + optional_size, section_count * 40, path)
		sections = []
		for index in range(section_count):
			section = section_data[index * 40:(index + 1) * 40]
			virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", section, 8)
			sections.append((virtual_address, max(virtual_size, raw_size), raw_size, raw_offset))

		def directory(index):
			entry_offset = directory_offset + index * 8
			if index >= directory_count or entry_offset + 8 > optional_size:
				return (0, 0)
			return struct.unpack_from("<II", optional, entry_offset)

		def rva_to_offset(rva):
			if rva < size_of_headers:
				return rva
			for virtual_address, mapped_size, raw_size, raw_offset in sections:
				if virtual_address <= rva < virtual_address + mapped_size:
					delta = rva - virtual_address
					if delta >= raw_size:
						break
					return raw_offset + delta
			raise PEFormatError("unmapped PE RVA 0x{:x} in {}".format(rva, path))

		def read_name(name_rva):
			offset = rva_to_offset(name_rva)
			source.seek(offset)
			name = bytearray()
			while len(name) < 4096:
				character = source.read(1)
				if not character:
					raise PEFormatError("truncated import name in {}".format(path))
				if character == b"\0":
					break
				name.extend(character)
			else:
				raise PEFormatError("oversized import name in {}".format(path))
			try:
				return name.decode("ascii").casefold()
			except UnicodeDecodeError as error:
				raise PEFormatError("non-ASCII import name in {}".format(path)) from error

		imports = set()
		import_rva, import_size = directory(1)
		if import_rva:
			descriptor_offset = rva_to_offset(import_rva)
			limit = min(max(import_size // 20 + 1, 1), 65536)
			for index in range(limit):
				descriptor = read_exact(source, descriptor_offset + index * 20, 20, path)
				if descriptor == b"\0" * 20:
					break
				name_rva = struct.unpack_from("<I", descriptor, 12)[0]
				if name_rva:
					imports.add(read_name(name_rva))
			else:
				raise PEFormatError("unterminated import directory in {}".format(path))

		delay_rva, delay_size = directory(13)
		if delay_rva:
			descriptor_offset = rva_to_offset(delay_rva)
			limit = min(max(delay_size // 32 + 1, 1), 65536)
			for index in range(limit):
				descriptor = read_exact(source, descriptor_offset + index * 32, 32, path)
				if descriptor == b"\0" * 32:
					break
				attributes, name_address = struct.unpack_from("<II", descriptor, 0)
				name_rva = name_address if attributes & 1 else name_address - image_base
				if name_rva > 0:
					imports.add(read_name(name_rva))
			else:
				raise PEFormatError("unterminated delay-import directory in {}".format(path))

	return machine, imports


def stat_signature(path):
	info = path.stat()
	return {"size": info.st_size, "mtime_ns": info.st_mtime_ns}


def signature_map(paths):
	return {str(path): stat_signature(path) for path in sorted(paths, key=lambda item: str(item).casefold())}


def path_list(paths):
	return [str(path) for path in sorted(paths, key=lambda item: str(item).casefold())]


def file_digest(path):
	digest = hashlib.sha256()
	with path.open("rb") as source:
		while True:
			chunk = source.read(READ_SIZE)
			if not chunk:
				break
			digest.update(chunk)
	return digest.hexdigest()


def same_content(source, destination):
	try:
		if source.stat().st_size != destination.stat().st_size:
			return False
	except FileNotFoundError:
		return False
	with source.open("rb") as left, destination.open("rb") as right:
		while True:
			left_chunk = left.read(READ_SIZE)
			right_chunk = right.read(READ_SIZE)
			if left_chunk != right_chunk:
				return False
			if not left_chunk:
				return True


def copy_if_changed(source, destination):
	destination.parent.mkdir(parents=True, exist_ok=True)
	if same_content(source, destination):
		shutil.copystat(source, destination)
		return False

	temporary = None
	try:
		with tempfile.NamedTemporaryFile(
			prefix=".{}-".format(destination.name), suffix=".tmp", dir=destination.parent, delete=False
		) as output:
			temporary = Path(output.name)
		shutil.copy2(source, temporary)
		os.replace(temporary, destination)
		return True
	finally:
		if temporary is not None:
			try:
				temporary.unlink()
			except FileNotFoundError:
				pass


def canonical(path):
	return Path(path).expanduser().resolve()


def meson_install_prefix(build_root):
	options_path = build_root / "meson-info" / "intro-buildoptions.json"
	try:
		with options_path.open("r", encoding="utf-8") as source:
			options = json.load(source)
	except (FileNotFoundError, json.JSONDecodeError):
		return None
	for option in options:
		if option.get("name") == "prefix":
			return canonical(option["value"])
	return None


def discover_meson_seeds(build_root, install_dir):
	if build_root is None:
		return set(), set()
	manifest = build_root / "meson-info" / "intro-installed.json"
	configured_prefix = meson_install_prefix(build_root)
	if configured_prefix is None:
		return set(), set()
	try:
		with manifest.open("r", encoding="utf-8") as source:
			installed = json.load(source)
	except (FileNotFoundError, json.JSONDecodeError):
		return set(), set()

	seeds = set()
	root_dlls = set()
	for source_name, destination_name in installed.items():
		source = canonical(source_name)
		if source.suffix.casefold() not in (".exe", ".dll") or not source.is_file():
			continue
		destination_text = str(destination_name).replace("{prefix}", str(configured_prefix))
		destination = canonical(destination_text)
		try:
			relative = destination.relative_to(configured_prefix)
		except ValueError as error:
			raise RuntimeError(
				"Meson install destination escapes its configured prefix: {}".format(destination)
			) from error
		installed_path = canonical(install_dir / relative)
		if not installed_path.is_file():
			continue
		seeds.add(installed_path)
		if installed_path.suffix.casefold() == ".dll" and installed_path.parent == install_dir:
			root_dlls.add(installed_path)
	return seeds, root_dlls


def discover_seeds(install_dir, build_root, prefix_dlls, prior_owned_paths):
	meson_seeds, root_dlls = discover_meson_seeds(build_root, install_dir)
	seeds = set(meson_seeds)
	ericw_seeds = set()
	for pattern in ("ericw/*.exe", "ericw/*.dll"):
		ericw_seeds.update(canonical(path) for path in install_dir.glob(pattern) if path.is_file())
	seeds.update(ericw_seeds)

	# The editor loads every module/plugin present in these directories, including
	# user-added and stale files that are not represented by Meson's manifest.
	for pattern in ("*.exe", "modules/*.dll", "plugins/*.dll"):
		seeds.update(canonical(path) for path in install_dir.glob(pattern) if path.is_file())
	for path in install_dir.glob("*.dll"):
		root_dll = canonical(path)
		if root_dll in root_dlls:
			continue
		if path.name.casefold() in prefix_dlls or root_dll in prior_owned_paths:
			continue
		seeds.add(root_dll)
		root_dlls.add(root_dll)
	return seeds, root_dlls, ericw_seeds


def index_by_name(paths, description):
	indexed = {}
	for path in sorted(paths, key=lambda item: str(item).casefold()):
		name = path.name.casefold()
		previous = indexed.get(name)
		if previous is not None and previous != path:
			raise RuntimeError("ambiguous {} named {}: {} and {}".format(description, path.name, previous, path))
		indexed[name] = path
	return indexed


def discover_plugins(mingw_prefix, qt_major):
	plugin_root = mingw_prefix / "share" / "qt{}".format(qt_major) / "plugins"
	if not plugin_root.is_dir():
		raise RuntimeError("Qt {} plugin root does not exist: {}".format(qt_major, plugin_root))
	plugins = set()
	plugin_groups = COMMON_PLUGIN_GROUPS + (QT5_PLUGIN_GROUPS if qt_major == "5" else ())
	for group in plugin_groups:
		plugins.update(canonical(path) for path in (plugin_root / group).glob("*.dll") if path.is_file())
	if not any(path.parent.name == "platforms" for path in plugins):
		raise RuntimeError("Qt platform plugins were not found under {}".format(plugin_root))
	return plugin_root, plugins


def infer_qt_major(seeds):
	majors = set()
	for seed in sorted(seeds, key=lambda item: str(item).casefold()):
		_, imports = read_pe_imports(seed)
		if "qt6core.dll" in imports:
			majors.add("6")
		if "qt5core.dll" in imports:
			majors.add("5")
	if len(majors) != 1:
		raise RuntimeError("could not infer one Qt major version from installed binaries")
	return majors.pop()


def system_import(name):
	if name.startswith(API_SET_PREFIXES):
		return True
	windows_root = os.environ.get("WINDIR") or os.environ.get("SystemRoot")
	if windows_root:
		for directory in (Path(windows_root) / "System32", Path(windows_root) / "SysWOW64"):
			if (directory / name).is_file():
				return True
	return False


def resolve_closure(seeds, plugin_sources, prefix_dlls, root_dlls, ericw_seeds):
	root_dlls_by_name = index_by_name(root_dlls, "application root DLL")
	ericw_dlls_by_name = index_by_name(
		(path for path in ericw_seeds if path.suffix.casefold() == ".dll"), "ericw DLL"
	)
	pending = set(seeds) | set(plugin_sources)
	scanned = set()
	resolved = set()
	expected_machine = None
	missing = []

	while pending:
		path = pending.pop()
		if path in scanned:
			continue
		machine, imports = read_pe_imports(path)
		if expected_machine is None:
			expected_machine = machine
		elif machine != expected_machine:
			raise RuntimeError("PE architecture mismatch: {}".format(path))
		scanned.add(path)
		for name in imports:
			target = ericw_dlls_by_name.get(name) if path in ericw_seeds else None
			if target is None:
				target = root_dlls_by_name.get(name)
			if target is not None:
				if target not in scanned:
					pending.add(target)
				continue
			target = prefix_dlls.get(name)
			if target is not None:
				resolved.add(target)
				if target not in scanned:
					pending.add(target)
				continue
			if not system_import(name):
				missing.append((path, name))
	if missing:
		details = "\n".join("  {} imports {}".format(path, name) for path, name in missing)
		raise RuntimeError("unresolved non-system runtime imports:\n{}".format(details))
	return resolved, len(scanned)


def safe_destination(install_dir, relative_name):
	if not isinstance(relative_name, str):
		raise RuntimeError("unsafe non-text runtime manifest path")
	if "\\" in relative_name or ":" in relative_name or "\0" in relative_name:
		raise RuntimeError("unsafe runtime manifest path: {}".format(relative_name))
	relative = PurePosixPath(relative_name)
	parts = relative.parts
	valid_root_dll = len(parts) == 1 and parts[0].casefold().endswith(".dll")
	valid_qt_plugin = (
		len(parts) == 5
		and parts[0].casefold() == "share"
		and parts[1].casefold() in ("qt5", "qt6")
		and parts[2].casefold() == "plugins"
		and parts[4].casefold().endswith(".dll")
	)
	if relative.is_absolute() or ".." in parts or not (valid_root_dll or valid_qt_plugin):
		raise RuntimeError("unsafe runtime manifest path: {}".format(relative_name))
	destination = canonical(install_dir.joinpath(*parts))
	try:
		destination.relative_to(install_dir)
	except ValueError as error:
		raise RuntimeError("runtime manifest path escapes install root: {}".format(relative_name)) from error
	return destination


def load_manifest(path):
	try:
		with path.open("r", encoding="utf-8") as source:
			return json.load(source)
	except (FileNotFoundError, json.JSONDecodeError):
		return None


def valid_owned_entries(manifest, install_dir):
	if not isinstance(manifest, dict) or not isinstance(manifest.get("owned"), dict):
		return {}
	entries = {}
	for relative_name, signature in manifest["owned"].items():
		try:
			destination = safe_destination(install_dir, relative_name)
		except RuntimeError:
			continue
		entries[relative_name] = (destination, signature)
	return entries


def signatures_match(recorded):
	if not isinstance(recorded, dict):
		return False
	for path_name, signature in recorded.items():
		try:
			if stat_signature(Path(path_name)) != signature:
				return False
		except OSError:
			return False
	return True


def manifest_is_current(manifest, context, seeds, root_dlls, ericw_seeds, plugins, install_dir):
	if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA_VERSION:
		return False
	if manifest.get("context") != context:
		return False
	if manifest.get("seeds") != signature_map(seeds):
		return False
	if manifest.get("root_dlls") != path_list(root_dlls):
		return False
	if manifest.get("ericw_seeds") != path_list(ericw_seeds):
		return False
	if manifest.get("plugins") != signature_map(plugins):
		return False
	if not signatures_match(manifest.get("dependencies")):
		return False
	owned = manifest.get("owned")
	if not isinstance(owned, dict):
		return False
	for relative_name, signature in owned.items():
		try:
			if stat_signature(safe_destination(install_dir, relative_name)) != signature:
				return False
		except (OSError, RuntimeError):
			return False
	return True


def write_manifest(path, manifest):
	temporary = path.with_name(path.name + ".tmp.{}".format(os.getpid()))
	try:
		with temporary.open("x", encoding="utf-8", newline="\n") as output:
			json.dump(manifest, output, indent=2, sort_keys=True)
			output.write("\n")
			output.flush()
			os.fsync(output.fileno())
		os.replace(temporary, path)
	finally:
		try:
			temporary.unlink()
		except FileNotFoundError:
			pass


@contextmanager
def install_lock(path, timeout=30.0):
	path.parent.mkdir(parents=True, exist_ok=True)
	lock_file = path.open("a+b")
	if lock_file.tell() == 0:
		lock_file.write(b"\0")
		lock_file.flush()
	lock_file.seek(0)
	deadline = time.monotonic() + timeout
	if os.name == "nt":
		import msvcrt

		while True:
			try:
				msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
				break
			except OSError:
				if time.monotonic() >= deadline:
					lock_file.close()
					raise RuntimeError("timed out waiting for runtime install lock")
				time.sleep(0.1)
		try:
			yield
		finally:
			lock_file.seek(0)
			msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
			lock_file.close()
	else:
		import fcntl

		while True:
			try:
				fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
				break
			except BlockingIOError:
				if time.monotonic() >= deadline:
					lock_file.close()
					raise RuntimeError("timed out waiting for runtime install lock")
				time.sleep(0.1)
		try:
			yield
		finally:
			fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
			lock_file.close()


def parse_arguments():
	default_install = os.environ.get("MESON_INSTALL_DESTDIR_PREFIX") or os.environ.get("INSTALLDIR")
	parser = argparse.ArgumentParser()
	parser.add_argument("--install-dir", default=default_install)
	parser.add_argument("--mingw-prefix", default=os.environ.get("MINGW_PREFIX"))
	parser.add_argument("--qt-major", choices=("auto", "5", "6"), default=os.environ.get("QT_MAJOR", "auto"))
	parser.add_argument("--build-root", default=os.environ.get("MESON_BUILD_ROOT"))
	args = parser.parse_args()
	if not args.install_dir:
		parser.error("--install-dir or MESON_INSTALL_DESTDIR_PREFIX/INSTALLDIR is required")
	return args


def infer_mingw_prefix(value):
	if value:
		return canonical(value)
	compiler = shutil.which("c++") or shutil.which("g++")
	if not compiler:
		raise RuntimeError("cannot infer the active MSYS2 prefix because no C++ compiler is on PATH")
	return canonical(Path(compiler).parent.parent)


def run(args):
	install_dir = canonical(args.install_dir)
	mingw_prefix = infer_mingw_prefix(args.mingw_prefix)
	build_root = canonical(args.build_root) if args.build_root else None
	bin_dir = mingw_prefix / "bin"
	if not install_dir.is_dir():
		raise RuntimeError("install directory does not exist: {}".format(install_dir))
	if not bin_dir.is_dir():
		raise RuntimeError("MSYS2 runtime directory does not exist: {}".format(bin_dir))

	prefix_dlls = index_by_name(
		(canonical(path) for path in bin_dir.glob("*.dll") if path.is_file()), "MSYS2 runtime DLL"
	)
	manifest_path = install_dir / MANIFEST_NAME
	with install_lock(install_dir / LOCK_NAME):
		previous = load_manifest(manifest_path)
		previous_owned = valid_owned_entries(previous, install_dir)
		prior_owned_paths = {destination for destination, _ in previous_owned.values()}
		seeds, root_dlls, ericw_seeds = discover_seeds(
			install_dir, build_root, prefix_dlls, prior_owned_paths
		)
		if not seeds:
			raise RuntimeError("no installed application binaries were found in {}".format(install_dir))
		detected_qt_major = infer_qt_major(seeds)
		qt_major = detected_qt_major if args.qt_major == "auto" else args.qt_major
		if qt_major != detected_qt_major:
			raise RuntimeError(
				"configured Qt {} does not match Qt {} imported by the application".format(
					qt_major, detected_qt_major
				)
			)
		plugin_root, plugins = discover_plugins(mingw_prefix, qt_major)
		context = {
			"helper_sha256": file_digest(Path(__file__).resolve()),
			"mingw_prefix": str(mingw_prefix),
			"plugin_root": str(plugin_root),
			"qt_major": qt_major,
		}
		if manifest_is_current(
			previous, context, seeds, root_dlls, ericw_seeds, plugins, install_dir
		):
			print(
				"MSYS2 runtime is unchanged; kept {} managed files.".format(len(previous["owned"]))
			)
			return

		dependencies, scanned_count = resolve_closure(
			seeds, plugins, prefix_dlls, root_dlls, ericw_seeds
		)
		desired = {}
		for source in dependencies:
			desired[source.name] = source
		for source in plugins:
			relative = source.relative_to(mingw_prefix).as_posix()
			desired[relative] = source

		copied = 0
		for relative_name, source in sorted(desired.items()):
			if copy_if_changed(source, safe_destination(install_dir, relative_name)):
				copied += 1

		removed = 0
		for relative_name, (destination, signature) in previous_owned.items():
			if relative_name in desired:
				continue
			if destination in seeds:
				continue
			try:
				if stat_signature(destination) == signature:
					destination.unlink()
					removed += 1
			except FileNotFoundError:
				pass

		owned = {
			relative_name: stat_signature(safe_destination(install_dir, relative_name))
			for relative_name in sorted(desired)
		}
		manifest = {
			"schema": SCHEMA_VERSION,
			"context": context,
			"seeds": signature_map(seeds),
			"root_dlls": path_list(root_dlls),
			"ericw_seeds": path_list(ericw_seeds),
			"plugins": signature_map(plugins),
			"dependencies": signature_map(dependencies),
			"owned": owned,
		}
		write_manifest(manifest_path, manifest)
		print(
			"MSYS2 runtime closure: {} PE files scanned, {} dependencies, {} Qt plugins; "
			"{} copied, {} removed.".format(
				scanned_count, len(dependencies), len(plugins), copied, removed
			)
		)


def main():
	try:
		run(parse_arguments())
		return 0
	except (OSError, PEFormatError, RuntimeError) as error:
		print("MSYS2 runtime staging failed: {}".format(error), file=sys.stderr)
		return 1


if __name__ == "__main__":
	sys.exit(main())
