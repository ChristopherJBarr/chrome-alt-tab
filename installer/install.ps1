param([switch]$Quiet, [string]$ReplaceDevelopmentManifest)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$destination = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs\ChromeAltTab'))
$source = [IO.Path]::GetFullPath($PSScriptRoot)
$installedExe = Join-Path $destination 'chrome-alt-tab-switcher.exe'
$command = '"' + $installedExe + '" --broker'
if ($command.Length -gt 260) { throw 'Installation path is too long for Windows startup.' }
if (-not [Environment]::Is64BitOperatingSystem -or [Environment]::OSVersion.Version.Build -lt 22000) { throw 'Windows 11 x64 is required.' }
if ($source -eq $destination) { throw 'Extract a new release ZIP elsewhere before installing or upgrading.' }
$required = @('chrome-alt-tab-switcher.exe', 'extension\manifest.json', 'uninstall.ps1', 'QUICKSTART.html', 'LICENSE', 'payload.json')
foreach ($relative in $required) { if (-not (Test-Path -LiteralPath (Join-Path $source $relative) -PathType Leaf)) { throw "Incomplete download: $relative" } }
$payload = Get-Content -LiteralPath (Join-Path $source 'payload.json') -Raw | ConvertFrom-Json
if ($payload.product -ne 'chrome-alt-tab' -or $payload.version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release manifest.' }
# Validate every archive member before copying any files. The hash list checks
# corruption, not publisher identity: the preview release is not code-signed.
foreach ($item in $payload.files) {
	$path = [IO.Path]::GetFullPath((Join-Path $source $item.path))
	if (-not $path.StartsWith($source + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid payload path.' }
	if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $item.sha256) { throw "Damaged release file: $($item.path)" }
}
$markerPath = Join-Path $destination 'installation.json'
if (Test-Path -LiteralPath $destination) {
	$entries = @((Get-Item -LiteralPath $destination)) + @(Get-ChildItem -LiteralPath $destination -Force -Recurse)
	if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw 'Installation contains directory links; refusing to overwrite it.' }
	if (-not (Test-Path -LiteralPath $markerPath)) { throw 'Destination already exists and is not a recognised installation.' }
	$marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
	if ($marker.product -ne 'chrome-alt-tab' -or $marker.installRoot -ne $destination) { throw 'Installation ownership check failed.' }
}
$hostKey = 'Software\Google\Chrome\NativeMessagingHosts\org.chrome_alt_tab.host'
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry32)
try {
	$existing = $registry.OpenSubKey($hostKey)
	$existingManifest = $null
	if ($existing) { try { $existingManifest = [string]$existing.GetValue('') } finally { $existing.Dispose() } }
	$manifestPath = Join-Path $destination 'native-host.json'
	if ($existingManifest -and $existingManifest -ne $manifestPath) {
		if (-not $ReplaceDevelopmentManifest -or [IO.Path]::GetFullPath($ReplaceDevelopmentManifest) -ne $existingManifest) {
			throw 'A development copy or another installation is registered. Unregister that copy first; see QUICKSTART.html.'
		}
	}
	# Stop only the executable in our exact, validated installation directory.
	$installedExe = Join-Path $destination 'chrome-alt-tab-switcher.exe'
	$running = @(Get-Process chrome-alt-tab-switcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $installedExe })
	foreach ($process in $running) { Stop-Process -Id $process.Id -ErrorAction Stop; if (-not $process.WaitForExit(5000)) { throw 'Previous helper did not stop.' } }
	New-Item -ItemType Directory -Path $destination -Force | Out-Null
	foreach ($item in $payload.files) {
		$target = Join-Path $destination $item.path
		New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
		Copy-Item -LiteralPath (Join-Path $source $item.path) -Destination $target -Force
	}
	$encoding = [Text.UTF8Encoding]::new($false)
	$extension = Get-Content -LiteralPath (Join-Path $destination 'extension\manifest.json') -Raw | ConvertFrom-Json
	$hash = [Security.Cryptography.SHA256]::Create()
	try { $digest = $hash.ComputeHash([Convert]::FromBase64String($extension.key)) } finally { $hash.Dispose() }
	$extensionId = -join ($digest[0..15] | ForEach-Object { [char](97 + ($_ -shr 4)); [char](97 + ($_ -band 15)) })
	$manifest = [ordered]@{ name='org.chrome_alt_tab.host'; description='Chrome Alt+Tab'; path=$installedExe; type='stdio'; allowed_origins=@("chrome-extension://$extensionId/") }
	[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 4), $encoding)
	[IO.File]::WriteAllText($markerPath, (@{ product='chrome-alt-tab'; version=$payload.version; installRoot=$destination } | ConvertTo-Json), $encoding)
	$key = $registry.CreateSubKey($hostKey)
	try { $key.SetValue('', $manifestPath) } finally { $key.Dispose() }
	$run = $registry.CreateSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
	try { $run.SetValue('ChromeAltTab', $command) } finally { $run.Dispose() }
	$uninstall = $registry.CreateSubKey('Software\Microsoft\Windows\CurrentVersion\Uninstall\ChromeAltTab')
	try {
		$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
		$uninstall.SetValue('DisplayName', 'Chrome Alt+Tab (preview)')
		$uninstall.SetValue('DisplayVersion', $payload.version)
		$uninstall.SetValue('Publisher', 'chrome-alt-tab contributors')
		$uninstall.SetValue('InstallLocation', $destination)
		$uninstall.SetValue('DisplayIcon', $installedExe)
		$uninstall.SetValue('UninstallString', '"' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $destination 'uninstall.ps1') + '"')
		$uninstall.SetValue('NoModify', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
		$uninstall.SetValue('NoRepair', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
	} finally { $uninstall.Dispose() }
	$shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'Chrome Alt+Tab.lnk'
	$shell = New-Object -ComObject WScript.Shell
	try {
		$shortcut = $shell.CreateShortcut($shortcutPath)
		$shortcut.TargetPath = $installedExe
		$shortcut.WorkingDirectory = $destination
		$shortcut.Description = 'Start Chrome Alt+Tab; use its tray icon to pause or quit.'
		$shortcut.Save()
	} finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null }
	Start-Process -FilePath $installedExe -ArgumentList '--broker' -WindowStyle Hidden
	Write-Output "Installed Chrome Alt+Tab $($payload.version). It starts automatically at sign-in."
	Write-Output "One-time Chrome setup: Load unpacked from $destination\extension in each profile."
	Write-Output 'Pause or quit using the tray icon (possibly under ^). Uninstall from Windows Settings > Apps > Installed apps.'
	if (-not $Quiet) { Start-Process -FilePath (Join-Path $destination 'QUICKSTART.html') }
} finally { $registry.Dispose() }
