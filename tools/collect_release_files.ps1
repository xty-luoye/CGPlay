[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputDir = "",
    [string]$Python = "python"
)
$ErrorActionPreference = "Stop"
if (-not $OutputDir) { $OutputDir = Join-Path $ProjectRoot "build_win_full\release_source" }
# Source collection never clears an existing directory or collects installers,
# credentials, build products, media or tests/artifacts. Use a fresh destination.
if (Test-Path -LiteralPath $OutputDir) { throw "Source output already exists; choose a fresh directory: $OutputDir" }
$program = @"
import json, pathlib, shutil, sys
sys.path.insert(0, str(pathlib.Path(sys.argv[1]) / 'tools'))
from prepare_release_sources import public_source_files
root=pathlib.Path(sys.argv[1]).resolve(strict=True)
out=pathlib.Path(sys.argv[2]).absolute()
if out.resolve().is_relative_to(root / 'src') or root.is_relative_to(out.resolve()):
    raise ValueError('Source output overlaps application input')
files=public_source_files(root)
out.mkdir(parents=True,exist_ok=False)
for source in files:
    destination=out/source.relative_to(root)
    destination.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(source,destination)
print(json.dumps({'passed':True,'files':len(files),'output':str(out),'backgroundMode':True,'platformVisibleTopLevelWindows':0},indent=2))
"@
& $Python -c $program $ProjectRoot $OutputDir
if ($LASTEXITCODE -ne 0) { throw "Public source collection failed." }
