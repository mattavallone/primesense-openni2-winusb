param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [Parameter(Mandatory = $true)]
    [string]$OutputDriverDir
)

$ErrorActionPreference = 'Stop'
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$OutputDriverDir = [System.IO.Path]::GetFullPath($OutputDriverDir)
$projectDir = $PSScriptRoot
$openniCommit = '115cf06c6efea32304182d293eca16ca883c9150'
$sourceDir = Join-Path $BuildDir '_deps\OpenNI2'
$transportSource = Join-Path $projectDir 'primesense_winusb_xnusb.cpp'

if (-not (Test-Path -LiteralPath (Join-Path $sourceDir '.git'))) {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    & git clone --depth 1 https://github.com/OpenNI/OpenNI2.git $sourceDir
    if ($LASTEXITCODE -ne 0) {
        throw 'Cloning OpenNI2 failed.'
    }
}

$checkedOutCommit = (& git -C $sourceDir rev-parse HEAD).Trim()
if ($checkedOutCommit -ne $openniCommit) {
    throw "OpenNI2 source revision mismatch: expected $openniCommit, got $checkedOutCommit."
}

$transportDestination = Join-Path $sourceDir `
    'ThirdParty\PSCommon\XnLib\Source\Win32\XnUSBWin32.cpp'
Copy-Item -LiteralPath $transportSource -Destination $transportDestination -Force

$projects = @(
    (Join-Path $sourceDir 'ThirdParty\PSCommon\XnLib\Source\XnLib.vcxproj'),
    (Join-Path $sourceDir 'Source\DepthUtils\DepthUtils.vcxproj'),
    (Join-Path $sourceDir 'Source\Drivers\PS1080\PS1080.vcxproj')
)
foreach ($project in $projects) {
    $contents = Get-Content -LiteralPath $project -Raw
    $updated = $contents.Replace(
        '<TreatWarningAsError>true</TreatWarningAsError>',
        '<TreatWarningAsError>false</TreatWarningAsError>')
    if ($updated -ne $contents) {
        Set-Content -LiteralPath $project -Value $updated -Encoding UTF8
    }
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$visualStudio = (& $vswhere -latest -products '*' -property installationPath).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($visualStudio)) {
    throw 'A Visual Studio C++ Build Tools installation was not found.'
}
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
$msbuild = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $vcvars) -or
    -not (Test-Path -LiteralPath $msbuild)) {
    throw 'The Visual Studio x64 compiler or MSBuild is unavailable.'
}

function Invoke-OpenNIProjectBuild {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)

    $command = 'call "{0}" >nul && "{1}" "{2}" /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142 /p:SolutionDir={3}\ /v:minimal' -f `
        $vcvars, $msbuild, $ProjectPath, $sourceDir
    & $env:ComSpec /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "OpenNI2 build failed for $ProjectPath."
    }
}

foreach ($project in $projects) {
    Invoke-OpenNIProjectBuild -ProjectPath $project
}

$builtDriver = Join-Path $sourceDir 'Bin\x64-Release\OpenNI2\Drivers\PS1080.dll'
if (-not (Test-Path -LiteralPath $builtDriver)) {
    throw "The PS1080 build did not produce $builtDriver."
}
New-Item -ItemType Directory -Force -Path $OutputDriverDir | Out-Null
Copy-Item -LiteralPath $builtDriver `
    -Destination (Join-Path $OutputDriverDir 'PS1080.dll') -Force
Copy-Item -LiteralPath (Join-Path $projectDir 'PS1080.ini') `
    -Destination (Join-Path $OutputDriverDir 'PS1080.ini') -Force

Write-Output "WinUSB-enabled PS1080 plugin installed to $OutputDriverDir."
