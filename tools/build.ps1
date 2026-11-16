param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw '找不到 VS2022 MSBuild，请安装 C++ 桌面开发及 v143 MFC x86/x64 组件。' }
& $msbuild (Join-Path $workspace 'CodexSessionCleaner.sln') "/p:Configuration=$Configuration" '/p:Platform=x64' /m /nologo /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw "编译失败，退出码 $LASTEXITCODE" }
& (Join-Path $workspace "bin\x64\$Configuration\SessionStoreTests.exe")
if ($LASTEXITCODE -ne 0) { throw '核心测试失败。' }
