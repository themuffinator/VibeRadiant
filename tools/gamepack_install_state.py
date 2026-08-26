#!/usr/bin/env python3
"""Validate the content-addressed state of an installed gamepack tree."""

import argparse
import hashlib
import json
import os
import platform
import stat
import sys
from pathlib import Path


SCHEMA_VERSION = 1
STATE_NAME = ".viberadiant-gamepacks-state.json"
READ_SIZE = 1024 * 1024


def hash_field(digest, value):
	data = value if isinstance(value, bytes) else str(value).encode("utf-8")
	digest.update(len(data).to_bytes(8, "little"))
	digest.update(data)


def hash_file(digest, path):
	with path.open("rb") as source:
		while True:
			chunk = source.read(READ_SIZE)
			if not chunk:
				break
			digest.update(chunk)


def tree_digest(root, excluded_names=()):
	root = root.resolve()
	if not root.is_dir():
		raise OSError("tree does not exist: {}".format(root))

	digest = hashlib.sha256()
	entries = sorted(root.rglob("*"), key=lambda path: path.relative_to(root).as_posix())
	for path in entries:
		relative = path.relative_to(root).as_posix()
		if relative in excluded_names:
			continue

		info = path.lstat()
		hash_field(digest, relative)
		hash_field(digest, stat.S_IMODE(info.st_mode))
		if stat.S_ISLNK(info.st_mode):
			hash_field(digest, "link")
			hash_field(digest, os.readlink(path))
		elif stat.S_ISDIR(info.st_mode):
			hash_field(digest, "directory")
		elif stat.S_ISREG(info.st_mode):
			hash_field(digest, "file")
			hash_field(digest, info.st_size)
			hash_file(digest, path)
		else:
			raise OSError("unsupported gamepack entry: {}".format(path))
	return digest.hexdigest()


def input_digest(source, transforms, parameters):
	digest = hashlib.sha256()
	hash_field(digest, "viberadiant-gamepacks-input-v{}".format(SCHEMA_VERSION))
	hash_field(digest, platform.python_implementation())
	hash_field(digest, platform.python_version())
	hash_field(digest, tree_digest(source))
	for transform in transforms:
		transform = transform.resolve()
		hash_field(digest, transform.name)
		hash_file(digest, transform)
	for parameter in parameters:
		hash_field(digest, parameter)
	return digest.hexdigest()


def load_state(path):
	try:
		with path.open("r", encoding="utf-8") as source:
			return json.load(source)
	except (FileNotFoundError, json.JSONDecodeError):
		return None


def check_state(source, destination, transforms, parameters):
	state = load_state(destination / STATE_NAME)
	if not isinstance(state, dict) or state.get("schema") != SCHEMA_VERSION:
		return False
	if state.get("input_digest") != input_digest(source, transforms, parameters):
		return False
	return state.get("output_digest") == tree_digest(destination, (STATE_NAME,))


def write_state(source, destination, transforms, parameters, expected_input):
	current_input = input_digest(source, transforms, parameters)
	if expected_input is not None and current_input != expected_input:
		raise RuntimeError("gamepack source or transforms changed while staging")
	state = {
		"schema": SCHEMA_VERSION,
		"input_digest": current_input,
		"output_digest": tree_digest(destination, (STATE_NAME,)),
	}
	state_path = destination / STATE_NAME
	temporary = destination / (STATE_NAME + ".tmp.{}".format(os.getpid()))
	try:
		with temporary.open("x", encoding="utf-8", newline="\n") as output:
			json.dump(state, output, indent=2, sort_keys=True)
			output.write("\n")
			output.flush()
			os.fsync(output.fileno())
		os.replace(temporary, state_path)
	finally:
		try:
			temporary.unlink()
		except FileNotFoundError:
			pass


def parse_arguments():
	parser = argparse.ArgumentParser()
	parser.add_argument("mode", choices=("check", "input", "write"))
	parser.add_argument("--source", type=Path, required=True)
	parser.add_argument("--destination", type=Path)
	parser.add_argument("--transform", action="append", default=[], type=Path)
	parser.add_argument("--parameter", action="append", default=[])
	parser.add_argument("--expected-input")
	args = parser.parse_args()
	if args.mode != "input" and args.destination is None:
		parser.error("--destination is required for check/write")
	return args


def main():
	args = parse_arguments()
	try:
		if args.mode == "check":
			return 0 if check_state(
				args.source, args.destination, args.transform, args.parameter
			) else 1
		if args.mode == "input":
			print(input_digest(args.source, args.transform, args.parameter))
			return 0
		write_state(
			args.source,
			args.destination,
			args.transform,
			args.parameter,
			args.expected_input,
		)
		return 0
	except (OSError, RuntimeError) as error:
		print("Gamepack install-state error: {}".format(error), file=sys.stderr)
		return 2


if __name__ == "__main__":
	sys.exit(main())
