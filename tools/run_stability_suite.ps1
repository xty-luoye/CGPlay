param(
    [switch]$Full,
    [string]$ReferenceUi = "C:\Users\1\Desktop\UI.png"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Python = Join-Path $Root "build_win_full\package\CGPlay\runtime\python\python.exe"
$Script = Join-Path $Root "tools\stability_suite.py"

$Args = @($Script, "--reference-ui", $ReferenceUi)
if (-not $Full) {
    $Args += "--quick"
}

& $Python @Args
exit $LASTEXITCODE
