param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\out\HP2Display.asi'),
    [string]$GameRoot,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
if ($Verify) {
    # The test harness uses known x86 call sites from this exact executable.
    if (-not $GameRoot) { throw 'Supply -GameRoot when running -Verify.' }
    $game = (Resolve-Path -LiteralPath $GameRoot).Path
    $exe = Join-Path $game 'NFSHP2.exe'
    $expectedHash = '3A08F1051319DC83341A63E61E7184496D3B92F7843712B195456D86B7F262C8'
    if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $expectedHash) {
        throw 'The game executable does not match the build used by the test harness.'
    }
}
$programFiles = [Environment]::GetFolderPath('ProgramFilesX86')
$vswhere = Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio x86 C++ tools were not found.' }

$msvcRoot = Join-Path $installation 'VC\Tools\MSVC'
$msvc = Get-ChildItem -LiteralPath $msvcRoot -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x86\cl.exe') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $msvc) { throw 'The x64-hosted x86 compiler was not found.' }

$sdk = Join-Path $programFiles 'Windows Kits\10'
$sdkInclude = Join-Path $sdk 'Include'
$version = Get-ChildItem -LiteralPath $sdkInclude -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $sdk "Lib\$($_.Name)\um\x86\kernel32.lib") } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
if (-not $version) { throw 'The x86 Windows SDK was not found.' }

$tempRoot = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).Path.TrimEnd('\')
$tempDir = Join-Path $tempRoot ('HP2Display-' + [Guid]::NewGuid().ToString('N'))
New-Item -Path $tempDir -ItemType Directory | Out-Null
$object = Join-Path $tempDir 'HP2Display.obj'
$source = Join-Path $PSScriptRoot 'HP2Display.cpp'
$binary = Join-Path $tempDir 'HP2Display.asi'
$compilerFlags = @('/LD')
$libraries = @('kernel32.lib')
if ($Verify) {
    $source = Join-Path $PSScriptRoot 'verify.cpp'
    $binary = Join-Path $tempDir 'verify.exe'
    $compilerFlags = @('/wd4740')
    $libraries += 'user32.lib'
}
try {
    & "$msvc\bin\Hostx64\x86\cl.exe" /nologo /std:c++17 /O2 /W4 /WX /EHsc /MT @compilerFlags `
        "/I$msvc/include" "/I$sdkInclude/$version/shared" "/I$sdkInclude/$version/um" `
        "/I$sdkInclude/$version/ucrt" "/Fo$object" "/Fe$binary" $source `
        /link /MACHINE:X86 "/LIBPATH:$msvc/lib/x86" "/LIBPATH:$sdk/Lib/$version/um/x86" `
        "/LIBPATH:$sdk/Lib/$version/ucrt/x86" @libraries
    if ($LASTEXITCODE -ne 0) { throw "HP2Display build failed with exit code $LASTEXITCODE." }
    if ($Verify) {
        # Load the game's Direct3D wrapper in isolation, with third-party ASIs disabled.
        $wrapper = Join-Path $tempDir 'd3d8.dll'
        Copy-Item -LiteralPath (Join-Path $game 'd3d8.dll') -Destination $wrapper
        @('[GlobalSets]', 'LoadPlugins=0', 'UseD3D8to9=1') | Set-Content -LiteralPath (Join-Path $tempDir 'global.ini') -Encoding ascii
        Push-Location -LiteralPath $tempDir
        try {
            & $binary (Join-Path $game 'NFSHP2.exe') $wrapper
            $verifyExit = $LASTEXITCODE
            $log = Join-Path $tempDir 'HP2Display.log'
            if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log }
            if ($verifyExit -ne 0) { throw "HP2Display verification failed with exit code $verifyExit." }
        } finally {
            Pop-Location
        }
    } else {
        $OutputPath = [IO.Path]::GetFullPath($OutputPath)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutputPath)) | Out-Null
        Copy-Item -LiteralPath $binary -Destination $OutputPath -Force
    }
} finally {
    $resolved = (Resolve-Path -LiteralPath $tempDir).Path
    if ($resolved -eq $tempDir -and [IO.Path]::GetDirectoryName($resolved) -eq $tempRoot) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
