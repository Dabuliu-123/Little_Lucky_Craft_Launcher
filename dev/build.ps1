$ErrorActionPreference = "Stop"
# Build script (dev/): sources in dev/src, objects in dev/build,
# output to ../release/<version>/ (exe + ini + readme + zip).
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$relRoot = Join-Path $root "..\release"

$verLine = Select-String -LiteralPath "$root\src\llcl.h" -Pattern '#define\s+LLCL_VERSION\s+L"([^"]+)"' | Select-Object -First 1
if (-not $verLine) { throw "LLCL_VERSION not found in src\llcl.h" }
$version = $verLine.Matches[0].Groups[1].Value
$outdir = Join-Path $relRoot $version

function Find-VcVars {
    $candidates = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        foreach ($i in $installs) { $candidates += (Join-Path $i "VC\Auxiliary\Build\vcvars64.bat") }
    }
    foreach ($base in @(${env:ProgramFiles}, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        foreach ($edition in @("Community", "Professional", "Enterprise", "BuildTools")) {
            $candidates += (Join-Path $base "Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat")
        }
    }
    foreach ($c in $candidates) { if ($c -and (Test-Path -LiteralPath $c)) { return $c } }
    return $null
}

$vcvars = Find-VcVars
if (-not $vcvars) { throw "vcvars64.bat not found (install Visual Studio 2022 with C++ desktop workload)" }
New-Item -ItemType Directory -Force -Path "$root\build", $outdir | Out-Null
$srcs = (Get-ChildItem -Path "$root\src\*.cpp" | Sort-Object Name | ForEach-Object { $_.FullName }) -join " "
$exe = Join-Path $outdir "LLcl.exe"
$bat = "@echo off`r`ncall `"$vcvars`" >nul 2>&1`r`ncd /d `"$root`"`r`ncl /nologo /O1 /MT /std:c++17 /EHsc /GR- /utf-8 /W3 /DUNICODE /D_UNICODE /Fo$root\build\ /Fe$exe $srcs /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF user32.lib comdlg32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib comctl32.lib advapi32.lib shlwapi.lib uuid.lib wininet.lib ws2_32.lib`r`n"
Set-Content -LiteralPath "$root\build\run.bat" -Value $bat -Encoding ASCII
cmd /c "$root\build\run.bat"
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

# Optional release payload templates (dev/release-files)
$tpl = Join-Path $root "release-files"
$zipFiles = @($exe)
foreach ($name in @("LLcl.ini", "README.txt")) {
    $src = Join-Path $tpl $name
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $outdir $name) -Force
        $zipFiles += (Join-Path $outdir $name)
    }
}

# Keep both the standalone exe and the zip inside each version folder.
$zip = Join-Path $outdir ("LLcl-$version-win64.zip")
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path $zipFiles -DestinationPath $zip -CompressionLevel Optimal
Get-Item $exe, $zip | Select-Object FullName, Length, LastWriteTime
