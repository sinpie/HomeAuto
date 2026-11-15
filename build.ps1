param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('New','Old')]
    [string]$Panel,
    [string]$Port = 'COM3',
    [switch]$Upload,
    [switch]$NoStub
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$cliCandidates = @(
    "$env:ProgramFiles\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
    "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
)
$cli = $cliCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $cli) {
    $foundCommand = Get-Command arduino-cli.exe -ErrorAction SilentlyContinue
    if ($foundCommand) { $cli = $foundCommand.Source }
}
if (-not $cli) { throw 'Arduino CLI not found. Install Arduino IDE 2.x or open HomePanel/HomePanel.ino in Arduino IDE.' }
$newPanel = if ($Panel -eq 'New') { '1' } else { '0' }
$fqbn = 'esp32:esp32:esp32p4:ChipVariant=postv3,USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled'
$buildDir = Join-Path $projectRoot "build-$Panel"
$configPath = Join-Path $projectRoot 'arduino-local.yaml'
$dataDir = Join-Path $env:LOCALAPPDATA 'Arduino15'
if (-not (Test-Path -LiteralPath (Join-Path $dataDir 'packages\esp32'))) {
    $profileDataDir = Join-Path $env:USERPROFILE 'AppData\Local\Arduino15'
    if (Test-Path -LiteralPath (Join-Path $profileDataDir 'packages\esp32')) { $dataDir = $profileDataDir }
}
$downloadsDir = Join-Path $projectRoot '.cache\downloads'
$userDir = Join-Path $projectRoot '.arduino'
$yaml = @"
directories:
  data: '$($dataDir.Replace("'", "''"))'
  downloads: '$($downloadsDir.Replace("'", "''"))'
  user: '$($userDir.Replace("'", "''"))'
network:
  connection_timeout: 5s
"@
[IO.File]::WriteAllText($configPath, $yaml, [Text.UTF8Encoding]::new($false))
$compileArgs = @('compile', '--config-file', $configPath, '--fqbn', $fqbn,
    '--build-path', $buildDir,
    '--build-property', "compiler.c.extra_flags=-DHOME_PANEL_NEW=$newPanel",
    '--build-property', "compiler.cpp.extra_flags=-DHOME_PANEL_NEW=$newPanel",
    (Join-Path $projectRoot 'HomePanel'))
& $cli @compileArgs
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed. No firmware was uploaded.' }
if ($Upload) {
    Write-Host "Uploading $Panel panel firmware to $Port using the P4 v3.2-compatible esptool. Close Serial Monitor first."
    $uploadArgs = @('-3', (Join-Path $projectRoot 'tools\upload_p4.py'), '--panel', $Panel, '--port', $Port, '--baud', '115200')
    if ($NoStub) { $uploadArgs += '--no-stub' }
    & py @uploadArgs
    if ($LASTEXITCODE -ne 0) { throw 'Upload failed. Firmware installation is not confirmed.' }
    Write-Host 'Upload completed. Verify the physical display and touch response.'
}
