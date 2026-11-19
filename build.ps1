# Builds RltRead.sys with the local VS2022 + WDK. No project files needed.
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Find-VS {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere not found" }
    $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $p) { throw "no VS with C++ tools" }
    return $p
}

$vsPath = Find-VS
$msvc = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvc) { throw "no MSVC toolchain" }
$vcBin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$vcInc = Join-Path $msvc.FullName 'include'

$kitRoot = 'C:\Program Files (x86)\Windows Kits\10'
$kitVer = Get-ChildItem (Join-Path $kitRoot 'Include') -Directory |
          Where-Object { Test-Path (Join-Path $_.FullName 'km\ntddk.h') } |
          Sort-Object Name -Descending | Select-Object -First 1
if (-not $kitVer) { throw "no WDK headers found" }
$kmInc = Join-Path $kitVer.FullName 'km'
$kmCrtInc = Join-Path $kitVer.FullName 'km\crt'
$ucrtInc = Join-Path $kitVer.FullName 'ucrt'
$sharedInc = Join-Path $kitVer.FullName 'shared'
$kmLib = Join-Path $kitRoot ('Lib\' + $kitVer.Name + '\km\x64')
if (-not (Test-Path (Join-Path $kmLib 'ntoskrnl.lib'))) { throw "no km libs at $kmLib" }

$env:PATH = "$vcBin;$env:PATH"
$env:INCLUDE = "$kmInc;$kmCrtInc;$ucrtInc;$sharedInc;$vcInc"
$env:LIB = "$kmLib"

$src = Join-Path $PSScriptRoot 'driver\RltRead.c'
$obj = Join-Path $out 'RltRead.obj'
$sys = Join-Path $out 'RltRead.sys'

Push-Location $out
try {
    & cl.exe /nologo /c /kernel /GS- /GR- /Gy /Zl /W4 /D_WIN64 /DAMD64  /D_AMD64_ "/Fo$obj" $src
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }

    & link.exe /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64 /OPT:REF /INCREMENTAL:NO "/OUT:$sys" $obj ntoskrnl.lib bufferoverflowk.lib
    if ($LASTEXITCODE -ne 0) { throw "link failed" }
}
finally { Pop-Location }

$f = Get-Item $sys
"built: $($f.FullName)  $($f.Length) bytes"
