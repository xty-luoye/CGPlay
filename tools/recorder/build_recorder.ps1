param([string]$ObsRoot='E:\rjkf\CGPlay-deps\obs-33.0.0')
$ErrorActionPreference='Stop'
$source=Split-Path -Parent $PSCommandPath
$vcvars='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $vcvars)) { throw 'Visual Studio BuildTools x64 compiler missing' }
$output=Join-Path $ObsRoot 'build'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$lines=@(
 '@echo off',
 ('call "{0}" >nul' -f $vcvars),
 ('python -B "{0}\patch_win_capture.py" "{1}"' -f $source,$ObsRoot),
 'if errorlevel 1 exit /b 1',
 ('python -B "{0}\patch_win_wasapi.py" "{1}"' -f $source,$ObsRoot),
 'if errorlevel 1 exit /b 1',
 ('python -B "{0}\prepare_runtime.py" --root "{1}"' -f $source,$ObsRoot),
 'if errorlevel 1 exit /b 1',
 ('cmake -S "{0}" -B "{1}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DOBS_ROOT="{2}"' -f $source,$output,($ObsRoot.Replace('\','/'))),
 'if errorlevel 1 exit /b 1',
 ('cmake --build "{0}"' -f $output),
 'if errorlevel 1 exit /b 1',
 ('python -B "{0}\prepare_runtime.py" --root "{1}" --finalize' -f $source,$ObsRoot),
 'exit /b %errorlevel%'
)
$script=Join-Path $output 'build-recorder.cmd'
[IO.File]::WriteAllLines($script,$lines,[Text.ASCIIEncoding]::new())
& $env:ComSpec /d /c $script
if ($LASTEXITCODE -ne 0) { throw "Recorder build failed: $LASTEXITCODE" }
