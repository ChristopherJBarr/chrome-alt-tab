# Package only reviewed source files; never recurse over a developer checkout.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repoRoot = Split-Path -Parent $PSScriptRoot
$files = @(
	'installer/install.cmd',
	'installer/install.ps1',
	'installer/uninstall.ps1',
	'installer/QUICKSTART.html',
	'scripts/package-release.ps1',
	'assets/app-icon.png',
	'assets/README.md',
	'scripts/export-icons.ps1',
	'native/resources/resource.h',
	'native/resources/version.rc.in',
	'native/resources/app.rc',
	'native/resources/app.ico',
	'extension/icons/icon-16.png',
	'extension/icons/icon-32.png',
	'extension/icons/icon-48.png',
	'extension/icons/icon-128.png',

	'.github/workflows/build.yml',
	'docs/architecture.md',
	'docs/chrome-test.md',
	'docs/development.md',
	'docs/protocol.md',
	'docs/switcher.md',
	'extension/README.md',
	'extension/background.js',
	'extension/manifest.json',
	'extension/model.js',
	'extension/popup.css',
	'extension/popup.html',
	'extension/popup.js',
	'extension/previews.js',
	'native/CMakeLists.txt',
	'native/src/preview.h',
	'native/src/protocol.h',
	'native/src/switcher.cpp',
	'native/src/switcher_ipc.h',
	'native/src/switcher_main.cpp',
	'native/src/switcher_model.h',
	'native/src/switcher_layout.h',
	'tests/package_audit.py',
	'tests/install_lifecycle.ps1',
	'tests/background.test.mjs',
	'tests/extension.test.mjs',
	'tests/fixtures/preview.jpg',
	'tests/previews.test.mjs',
	'tests/switcher_integration.py',
	'tests/switcher_model.cpp',
	'scripts/register-native-host.ps1',
	'scripts/package-source.ps1',
	'CMakeLists.txt',
	'README.md',
	'LICENSE',
	'CONTRIBUTING.md',
	'SECURITY.md',
	'.gitignore',
	'.editorconfig',
	'.gitattributes'
)
$outputDirectory = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$outputPath = Join-Path $outputDirectory 'chrome-alt-tab-source.zip'
$temporaryPath = Join-Path $outputDirectory ([Guid]::NewGuid().ToString() + '.zip')
try {
	$archive = [IO.Compression.ZipFile]::Open($temporaryPath, [IO.Compression.ZipArchiveMode]::Create)
	try {
		foreach ($file in $files) {
			$source = Join-Path $repoRoot $file
			if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing package source: $file" }
			[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $source, "chrome-alt-tab/$file", [IO.Compression.CompressionLevel]::Optimal) | Out-Null
		}
	} finally { $archive.Dispose() }
	Move-Item -LiteralPath $temporaryPath -Destination $outputPath -Force
	Write-Output "Created $outputPath ($($files.Count) source files)."
} finally {
	if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath }
}
