"""Check distribution integrity and accidental developer-path disclosure."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
from zipfile import ZipFile

with ZipFile(sys.argv[1]) as archive:
	names = archive.namelist()
	assert len(names) == len(set(names)), "Duplicate archive members"
	payload = json.loads(archive.read("payload.json"))
	assert payload["product"] == "chrome-alt-tab"
	expected = {item["path"] for item in payload["files"]} | {"payload.json"}
	assert set(names) == expected, "Unmanifested release files"
	for item in payload["files"]:
		name = item["path"]
		path = PurePosixPath(name)
		assert not path.is_absolute() and ".." not in path.parts and ":" not in name
		assert path.suffix.lower() not in {".log", ".pdb", ".obj", ".pem", ".pfx", ".key"}
		assert not any(part.startswith(".git") or part.startswith("build") for part in path.parts)
		data = archive.read(name)
		assert hashlib.sha256(data).hexdigest().lower() == item["sha256"].lower(), name
		for encoding in ["utf-8", "utf-16-le"]:
			assert "C:\\Users\\".encode(encoding).lower() not in data.lower(), f"Developer path in {name}"
			assert str(Path.cwd()).encode(encoding).lower() not in data.lower(), f"Checkout path in {name}"
	assert {"chrome-alt-tab-switcher.exe", "install.cmd", "install.ps1", "uninstall.ps1", "QUICKSTART.html", "extension/manifest.json", "LICENSE"} <= expected
print("PASS: release hashes, safe paths, required installation files and no developer paths")
