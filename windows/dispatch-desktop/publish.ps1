<#
.SYNOPSIS
  관제조작반 앱 배포 패키지 — 다른 PC 에서 설치 없이 실행되는 self-contained 게시(zip).

.DESCRIPTION
  dotnet publish(win-x64, self-contained) 산출물에 네이티브 cimsue.dll 이 요구하는 MSVC 14 런타임(msvcp140·vcruntime140·vcruntime140_1)을
  동봉해 zip 으로 만든다. 대상 PC 에는 .NET 도 VC 재배포 패키지도 필요 없다 — 풀고 CimsDispatch.exe 를 실행한다.
  Smart App Control 이 켜진 PC 는 미서명 apphost(CimsDispatch.exe)를 평판으로 차단하므로, Microsoft 서명 dotnet 뮤서(dotnet.exe + host/fxr/hostfxr.dll)를
  함께 동봉하고 그것으로 앱을 띄우는 CimsDispatch-run.cmd 를 둔다 — 두 진입점 모두 같은 폴더의 같은 파일을 실행한다.
  전제: sdk/windows 슈퍼빌드가 끝나 있어야 한다(build-win/sdk/bin/cimsue.dll + OpenSSL 런타임 둘). 없으면 중단한다.

.PARAMETER Configuration   Release(기본)|Debug
.PARAMETER NativeDir       cimsue.dll 위치 (기본 build-win/sdk/bin)
.PARAMETER OutDir          zip 을 둘 디렉터리 (기본 build-win/dist)
.PARAMETER NoZip           stage 디렉터리만 만들고 zip 은 생략

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File windows/dispatch-desktop/publish.ps1
  → build-win/dist/CimsDispatch-<버전>-win-x64.zip
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [string] $NativeDir = '',
    [string] $OutDir = '',
    [switch] $NoZip
)
$ErrorActionPreference = 'Stop'

$AppDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $AppDir '..\..')).Path
if ($NativeDir -eq '') { $NativeDir = Join-Path $RepoRoot 'build-win\sdk\bin' }
if ($OutDir -eq '') { $OutDir = Join-Path $RepoRoot 'build-win\dist' }
$Rid = 'win-x64'

# ── 0. 전제 확인 ──
$nativeFiles = 'cimsue.dll', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll'
foreach ($f in $nativeFiles) {
    if (-not (Test-Path (Join-Path $NativeDir $f))) {
        throw "네이티브 파일이 없다: $(Join-Path $NativeDir $f) — sdk/windows 슈퍼빌드를 먼저 돌린다 (sdk/windows/README.md '빌드')"
    }
}
$csproj = Join-Path $AppDir 'DispatchDesktop.csproj'
$version = ([xml](Get-Content $csproj)).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
if (-not $version) { $version = '0.0.0' }

$stage = Join-Path $OutDir "CimsDispatch-$version-$Rid"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# ── 1. dotnet publish (self-contained, 폴더 배치) ──
#   PublishSingleFile 은 쓰지 않는다 — WPF 는 단일 파일에서 네이티브 DLL 을 임시 폴더로 풀어 NativeLoader 의 runtimes/win-x64/native 탐색과 어긋난다.
#   PublishTrimmed 는 WPF 미지원. ReadyToRun 은 크기 대비 이득이 작아 끈다.
Write-Host "== dotnet publish ($Configuration, $Rid, self-contained) → $stage"
$nativeDirArg = $NativeDir.TrimEnd('\') + '\'
& dotnet publish $csproj -c $Configuration -r $Rid --self-contained true -o $stage `
    -p:PublishSingleFile=false -p:PublishReadyToRun=false -p:DebugType=none -p:DebugSymbols=false `
    "-p:CimsUeNativeDir=$nativeDirArg" -nologo -v:minimal
if ($LASTEXITCODE -ne 0) { throw "dotnet publish 실패 (exit $LASTEXITCODE)" }

# ── 2. MSVC 14 런타임 동봉 ──
#   cimsue.dll·OpenSSL 은 동적 CRT(/MD) 로 링크돼 msvcp140·vcruntime140·vcruntime140_1 을 요구한다(dumpbin /dependents).
#   .NET self-contained 런타임 자체는 CRT 를 정적으로 품어 따로 요구하지 않는다. 앱 디렉터리(exe 옆)에 두면 표준 DLL 검색 1순위로 잡힌다.
$crtNames = 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'
$crtDir = $null
$vsRoots = @("${env:ProgramFiles(x86)}\Microsoft Visual Studio", "${env:ProgramFiles}\Microsoft Visual Studio") | Where-Object { Test-Path $_ }
foreach ($root in $vsRoots) {
    $cands = Get-ChildItem -Path $root -Recurse -Directory -Filter 'Microsoft.VC143.CRT' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\Redist\MSVC\*\x64\Microsoft.VC143.CRT' } | Sort-Object FullName -Descending
    if ($cands) { $crtDir = $cands[0].FullName; break }
}
if ($crtDir) {
    Write-Host "== MSVC CRT ← $crtDir"
    foreach ($n in $crtNames) { Copy-Item (Join-Path $crtDir $n) (Join-Path $stage $n) -Force }
} else {
    Write-Warning "VC143 재배포 CRT 디렉터리를 찾지 못했다 — 대상 PC 에 'Microsoft Visual C++ 2015-2022 Redistributable (x64)' 가 필요하다"
}

# ── 2b. Smart App Control 우회 진입점 — 서명된 dotnet 뮤서 동봉 ──
#   SAC(WDAC 평판 정책)은 exe 만 파일 평판으로 막고 이 앱의 관리/네이티브 DLL 은 통과시킨다(개발 PC 실측). dotnet.exe 는 Microsoft 서명이라 항상 실행되고,
#   `dotnet.exe CimsDispatch.dll` 은 자기 옆 host\fxr\<ver>\hostfxr.dll → runtimeconfig 의 includedFrameworks 를 보고 앱 폴더의 self-contained 런타임을 쓴다.
#   버전은 게시된 런타임과 같은 것을 고른다(뮤서는 자기 폴더의 최고 버전 hostfxr 를 잡는다).
$rc = Get-Content (Join-Path $stage 'CimsDispatch.runtimeconfig.json') -Raw | ConvertFrom-Json
if (-not $rc.runtimeOptions.includedFrameworks) { throw 'runtimeconfig 에 includedFrameworks 가 없다 — self-contained 게시가 아니다' }
$rtVer = ($rc.runtimeOptions.includedFrameworks | Where-Object { $_.name -eq 'Microsoft.NETCore.App' } | Select-Object -First 1).version
$dotnetRoot = Split-Path (Get-Command dotnet).Source
$fxrDir = Join-Path $dotnetRoot "host\fxr\$rtVer"
if (-not (Test-Path $fxrDir)) {
    $fxrDir = (Get-ChildItem (Join-Path $dotnetRoot 'host\fxr') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).FullName
    Write-Warning "hostfxr $rtVer 이 설치돼 있지 않아 $(Split-Path $fxrDir -Leaf) 을 동봉한다"
}
$fxrLeaf = Split-Path $fxrDir -Leaf
New-Item -ItemType Directory -Force (Join-Path $stage "host\fxr\$fxrLeaf") | Out-Null
Copy-Item (Join-Path $fxrDir 'hostfxr.dll') (Join-Path $stage "host\fxr\$fxrLeaf\hostfxr.dll") -Force
Copy-Item (Join-Path $dotnetRoot 'dotnet.exe') (Join-Path $stage 'dotnet.exe') -Force
foreach ($signed in @((Join-Path $stage 'dotnet.exe'), (Join-Path $stage "host\fxr\$fxrLeaf\hostfxr.dll"))) {
    if ((Get-AuthenticodeSignature $signed).Status -ne 'Valid') { throw "$signed 의 서명이 유효하지 않다 — SAC 우회 진입점으로 쓸 수 없다" }
}
Write-Host "== dotnet 뮤서 동봉 (hostfxr $fxrLeaf)"
# 콘솔 창 없이: cmd → powershell(Hidden) → dotnet.exe(Hidden). 인자는 그대로 앱에 넘긴다(--ui-preview 등).
@'
@echo off
rem CIMS 관제조작반 — Smart App Control 이 CimsDispatch.exe 를 막을 때 쓰는 진입점(같은 앱, 서명된 dotnet 뮤서로 기동)
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command "Start-Process -FilePath '%~dp0dotnet.exe' -ArgumentList '\"%~dp0CimsDispatch.dll\" %*' -WorkingDirectory '%~dp0.' -WindowStyle Hidden"
'@ | Set-Content -Encoding Default (Join-Path $stage 'CimsDispatch-run.cmd')

# ── 3. 산출물 점검 ──
$mustHave = @('CimsDispatch.exe', 'CimsDispatch.dll', 'CimsUe.dll', 'directory.sample.csv') +
    ($nativeFiles | ForEach-Object { "runtimes\$Rid\native\$_" }) + @('e_sqlite3.dll', 'dotnet.exe', 'CimsDispatch-run.cmd')
$missing = $mustHave | Where-Object { -not (Test-Path (Join-Path $stage $_)) }
if ($missing) { throw "게시 산출물에 빠진 파일: $($missing -join ', ')" }

# ── 4. 대상 PC 안내문 ──
@"
CIMS 관제조작반 (CimsDispatch) $version — Windows x64

실행 (설치 없음)
  1. 이 폴더를 원하는 위치에 두고(예: C:\CimsDispatch) CimsDispatch.exe 를 실행한다.
  2. 첫 실행 시 로그인 창에서 CSC 주소·계정을 입력한다. 이후 설정·주소록·메시지 DB 는
     %APPDATA%\CIMS\dispatch-desktop 에 저장된다(앱 폴더는 읽기만 한다).
  3. 처음 통화·PTT 때 Windows 방화벽이 네트워크 접근을 물으면 허용한다(SIP/RTP UDP).

CimsDispatch.exe 가 "앱 제어 정책에서 이 파일을 차단했습니다" 로 열리지 않으면
  → 같은 폴더의 CimsDispatch-run.cmd 를 대신 실행한다. 같은 앱을 Microsoft 서명 런처(dotnet.exe)로 띄우는 것이다.
    Windows 11 의 Smart App Control 이 서명 없는 새 프로그램을 평판으로 막는 경우이며(설정 > 개인 정보 및 보안 >
    Windows 보안 > 앱 및 브라우저 컨트롤 > Smart App Control 에서 확인), 끄면 CimsDispatch.exe 도 그대로 실행된다.
    바탕화면 바로 가기가 필요하면 CimsDispatch-run.cmd 의 바로 가기를 만든다.

인터넷에서 받은 zip 은 SmartScreen 이 "알 수 없는 게시자" 경고를 한 번 낼 수 있다.
  → zip 파일 우클릭 > 속성 > "차단 해제" 후 풀거나, 경고 창에서 "추가 정보 > 실행" 을 택한다.

요구 사항
  - Windows 10 1809 이상 / Windows 11, 64비트.
  - .NET 런타임·Visual C++ 재배포 패키지 설치 불필요(모두 동봉).

폴더 구성
  CimsDispatch.exe             앱
  CimsDispatch-run.cmd         대체 진입점(위 참조) — dotnet.exe + host\fxr\ 와 짝
  runtimes\win-x64\native\     단말 SDK 네이티브(cimsue.dll) + OpenSSL
  msvcp140.dll 등              MSVC 런타임
  directory.sample.csv         주소록 CSV 예시(설정에서 다른 파일 지정 가능)
"@ | Set-Content -Encoding UTF8 (Join-Path $stage 'README.txt')

# ── 5. zip ──
if (-not $NoZip) {
    $zip = "$stage.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Write-Host "== zip → $zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
    $mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    Write-Host "완료: $zip ($mb MB)"
} else {
    Write-Host "완료: $stage"
}
