param(
    [Parameter(Mandatory = $true)]
    [string]$RedistDir,
    [Parameter(Mandatory = $true)]
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
$outputDrivers = Join-Path $OutputDir 'OpenNI2\Drivers'
$sourceDrivers = Join-Path $RedistDir 'OpenNI2\Drivers'

New-Item -ItemType Directory -Force -Path $outputDrivers | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $OutputDir 'OpenNI2.dll'))) {
    Copy-Item -LiteralPath (Join-Path $RedistDir 'OpenNI2.dll') `
        -Destination (Join-Path $OutputDir 'OpenNI2.dll')
}

foreach ($driverFile in Get-ChildItem -LiteralPath $sourceDrivers -File) {
    $destination = Join-Path $outputDrivers $driverFile.Name
    if (Test-Path -LiteralPath $destination) {
        continue
    }
    Copy-Item -LiteralPath $driverFile.FullName -Destination $destination
}

$configuration = Join-Path $outputDrivers 'PS1080.ini'
if (-not (Test-Path -LiteralPath $configuration)) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PS1080.ini') `
        -Destination $configuration
}
