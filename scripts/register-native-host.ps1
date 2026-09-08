param(
	[ValidatePattern('^[a-p]{32}$')]
	[string]$ExtensionId,
	[string]$BuildDirectory = 'build-release',
	[ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
	[string]$Configuration = 'Release',
	[switch]$Unregister
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $ExtensionId) {
	$extensionManifest = Get-Content -LiteralPath (Join-Path $repoRoot 'extension\manifest.json') -Raw | ConvertFrom-Json
	$hash = [Security.Cryptography.SHA256]::Create()
	try { $digest = $hash.ComputeHash([Convert]::FromBase64String($extensionManifest.key)) } finally { $hash.Dispose() }
	$ExtensionId = -join ($digest[0..15] | ForEach-Object { [char](97 + ($_ -shr 4)); [char](97 + ($_ -band 15)) })
}
$hostName = 'org.chrome_alt_tab.host'
$registryPath = "Software\Google\Chrome\NativeMessagingHosts\$hostName"
$view = [Microsoft.Win32.RegistryView]::Registry32
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
try {
	if ($Unregister) {
		$key = $registry.OpenSubKey($registryPath)
		if ($null -eq $key) { Write-Output 'Native host is already unregistered.'; return }
		try { $registeredPath = [string]$key.GetValue('') } finally { $key.Dispose() }
		$expectedPath = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\native-host.json'))
		if ($registeredPath -ne $expectedPath) { throw 'Registration belongs to another checkout; refusing to remove it.' }
		$registry.DeleteSubKey($registryPath, $false)
		Write-Output 'Removed this checkout native-host registration. Disable the extension in Chrome.'
		return
	}
	$buildRoot = Join-Path $repoRoot $BuildDirectory
	$hostFile = 'native\chrome-alt-tab-switcher.exe'
	$executable = Join-Path $buildRoot $hostFile
	if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { $executable = Join-Path $buildRoot "native\$Configuration\chrome-alt-tab-switcher.exe" }
	if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Build the Release native host first: $executable" }
	$executable = (Resolve-Path -LiteralPath $executable).Path
	$manifestDirectory = Join-Path $repoRoot 'build'
	New-Item -ItemType Directory -Path $manifestDirectory -Force | Out-Null
	$manifestPath = Join-Path $manifestDirectory 'native-host.json'
	$existing = $registry.OpenSubKey($registryPath)
	if ($null -ne $existing) {
		try { $oldPath = [string]$existing.GetValue('') } finally { $existing.Dispose() }
		if ($oldPath -ne $manifestPath) { throw "Host already registered by another checkout: $oldPath" }
	}
	$manifest = [ordered]@{
		name = $hostName
		description = 'Experimental Chrome Alt+Tab native host'
		path = $executable
		type = 'stdio'
		allowed_origins = @("chrome-extension://$ExtensionId/")
	}
	[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
	$key = $registry.CreateSubKey($registryPath)
	try { $key.SetValue('', $manifestPath, [Microsoft.Win32.RegistryValueKind]::String) } finally { $key.Dispose() }
	Write-Output "Registered $hostName for extension $ExtensionId under HKCU (32-bit view)."
	Write-Output 'Click the Chrome Alt+Tab extension button to connect.'
} finally {
	$registry.Dispose()
}
