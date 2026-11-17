$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$release = Join-Path $source 'release'
$override = Join-Path $source 'runtime\gvfg.dll'
$useOverride = $env:GVFG_BUILD_SOURCE_RUNTIME -ne '1' -and (Test-Path -LiteralPath $override -PathType Leaf)
$work = Join-Path $source ('.package-' + [guid]::NewGuid().ToString('N'))
$build = Join-Path $work 'build'
$stage = Join-Path $work 'stage'
$archive = Join-Path $release 'GVFG_CardTest-Windows-x64.zip'

function Invoke-CMake {
    & $env:CMAKE_EXE @args
    if ($LASTEXITCODE -ne 0) { throw "CMake failed with exit code $LASTEXITCODE." }
}

try {
    foreach ($path in @($env:CMAKE_EXE, $env:NINJA_EXE, (Join-Path $env:QT_ROOT 'lib\cmake\Qt6\Qt6Config.cmake'))) {
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Build tool or Qt kit not found: $path" }
    }
    New-Item -ItemType Directory -Path $work | Out-Null
    Invoke-CMake -S $source -B $build -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$env:QT_ROOT" "-DCMAKE_MAKE_PROGRAM=$env:NINJA_EXE"
    Invoke-CMake --build $build --parallel
    Invoke-CMake --install $build --prefix $stage

    # Deploy the matching MSVC runtime beside the executable for both processes.
    if (!$env:VCToolsRedistDir) { throw 'Visual Studio did not set VCToolsRedistDir.' }
    $crt = @(Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT')
    if ($crt.Count -ne 1) { throw 'Could not identify the x64 MSVC CRT redistribution folder.' }
    foreach ($name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path $crt[0].FullName $name) -PathType Leaf)) {
            throw "Missing x64 MSVC runtime file: $name"
        }
    }
    $bin = Join-Path $stage 'bin'
    Get-ChildItem -LiteralPath $crt[0].FullName -File -Filter '*.dll' |
        Copy-Item -Destination $bin -Force
    Write-Host "Bundled MSVCP140.dll: $((Get-Item -LiteralPath (Join-Path $bin 'msvcp140.dll')).VersionInfo.FileVersion)"

    if ($useOverride) {
        Copy-Item -LiteralPath $override -Destination (Join-Path $stage 'bin\gvfg.dll') -Force
        Write-Host "Using DLL override: $override"
    }
    $exe = Join-Path $stage 'bin\GVFG_CardTest.exe'
    foreach ($test in @('--self-test', '--ui-self-test')) {
        $process = Start-Process -FilePath $exe -ArgumentList $test -WindowStyle Hidden -PassThru
        if (!$process.WaitForExit(30000)) {
            Stop-Process -InputObject $process
            $process.WaitForExit()
            throw "$test timed out."
        }
        if ($process.ExitCode -ne 0) { throw "$test failed with exit code $($process.ExitCode)." }
    }

    New-Item -ItemType Directory -Path $release -Force | Out-Null
    $zip = Join-Path $work 'GVFG_CardTest-Windows-x64.zip'
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    if ([IO.File]::Exists($archive)) { [IO.File]::Replace($zip, $archive, (Join-Path $work 'previous.zip')) }
    else { [IO.File]::Move($zip, $archive) }
    Write-Host "Package: $archive"
} finally {
    if (Test-Path -LiteralPath $work) {
        $item = Get-Item -LiteralPath $work -Force
        if ($item.Parent.FullName -ne $source -or $item.Name -notmatch '^\.package-[a-f0-9]{32}$' -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unsafe temporary folder: $work"
        }
        Remove-Item -LiteralPath $work -Recurse -Force
    }
}
