param([string]$BuildDirectory = 'build-release', [string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo $BuildDirectory
$exe = Join-Path $build 'native\chrome-alt-tab-switcher.exe'
if (-not (Test-Path -LiteralPath $exe)) { $exe = Join-Path $build "native\$Configuration\chrome-alt-tab-switcher.exe" }
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build the Release helper first.' }
$version = (Get-Content -LiteralPath (Join-Path $repo 'extension\manifest.json') -Raw | ConvertFrom-Json).version
$stage = Join-Path $repo ('dist\package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
	Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'chrome-alt-tab-switcher.exe')
	$files = @('LICENSE', 'README.md', 'SECURITY.md', 'CONTRIBUTING.md',
		'docs/development.md', 'docs/architecture.md', 'docs/switcher.md', 'docs/protocol.md', 'docs/chrome-test.md',
		'extension/manifest.json', 'extension/background.js', 'extension/model.js', 'extension/previews.js',
		'extension/popup.js', 'extension/popup.html', 'extension/popup.css',
		'extension/icons/icon-16.png', 'extension/icons/icon-32.png', 'extension/icons/icon-48.png', 'extension/icons/icon-128.png')
	foreach ($file in $files) {
		$target = Join-Path $stage $file
		New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
		Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $target
	}
	foreach ($file in @('install.cmd', 'install.ps1', 'uninstall.ps1', 'QUICKSTART.html')) {
		Copy-Item -LiteralPath (Join-Path $repo "installer\$file") -Destination (Join-Path $stage $file)
	}
	$records = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
		@{ path=$_.FullName.Substring($stage.Length + 1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
	})
	[IO.File]::WriteAllText((Join-Path $stage 'payload.json'), (@{product='chrome-alt-tab'; version=$version; files=$records} | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
	$output = Join-Path $repo "dist\chrome-alt-tab-$version-windows-x64.zip"
	if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output }
	Add-Type -AssemblyName System.IO.Compression.FileSystem
	[IO.Compression.ZipFile]::CreateFromDirectory($stage, $output)
	$checksum = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($output)
	[IO.File]::WriteAllText(($output + '.sha256'), $checksum + "`n", [Text.UTF8Encoding]::new($false))
	Write-Output "Created $output"
} finally {
	$resolved = [IO.Path]::GetFullPath($stage)
	$dist = [IO.Path]::GetFullPath((Join-Path $repo 'dist')) + [IO.Path]::DirectorySeparatorChar
	if (-not $resolved.StartsWith($dist, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe staging directory.' }
	if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
