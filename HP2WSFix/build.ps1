param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\out\HP2WSFix25-verified')
)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\MSBuild.exe'
if (-not $msbuild) { throw 'Visual Studio MSBuild and the x86 C++ tools are required.' }

$output = [IO.Path]::GetFullPath($OutputDirectory)
$intermediate = Join-Path $output 'obj'
$project = Join-Path $PSScriptRoot 'HP2WSFix.vcxproj'
& $msbuild $project /nologo /v:minimal /p:Configuration=Release /p:Platform=Win32 "/p:OutDir=$output/" "/p:IntDir=$intermediate/"
if ($LASTEXITCODE -ne 0) { throw "HP2WSFix build failed with exit code $LASTEXITCODE." }
