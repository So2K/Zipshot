param(
    [Parameter(Mandatory=$true)][int]$TargetPid,
    [ValidateSet('full','region')][string]$Scenario='region',
    [int]$Count=30,[int]$Warmups=3,
    [int]$X=0,[int]$Y=0,[int]$Width=1280,[int]$Height=720,
    [Parameter(Mandatory=$true)][string]$SaveDir,
    [Parameter(Mandatory=$true)][string]$ResultPath,
    [string]$Controller='C:/ZipshotLab/input/capture_hotkeys.exe',
    [switch]$SaveEndpointFrame
)
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount') {throw 'Refusing screen/input access outside the disposable Sandbox guest.'}
$psi=New-Object Diagnostics.ProcessStartInfo
$psi.FileName=$Controller
$psi.Arguments=('{0} {1} {2} {3} {4} {5} {6} "{7}" "{8}" {9}' -f $TargetPid,$Scenario,$Count,$X,$Y,$Width,$Height,$SaveDir,$ResultPath,$Warmups)
if($SaveEndpointFrame){$psi.Arguments+=' snapshots'}
$psi.UseShellExecute=$false
$psi.CreateNoWindow=$true
$psi.RedirectStandardOutput=$true
$psi.RedirectStandardError=$true
$process=[Diagnostics.Process]::Start($psi)
$stdout=$process.StandardOutput.ReadToEndAsync()
$stderr=$process.StandardError.ReadToEndAsync()
if(-not $process.WaitForExit(900000)) {throw 'Guest capture controller exceeded 15-minute timeout.'}
$stdout.Result | Set-Content -LiteralPath ($ResultPath+'.stdout.txt')
$stderr.Result | Set-Content -LiteralPath ($ResultPath+'.stderr.txt')
if(Test-Path -LiteralPath $ResultPath) {Get-Content -LiteralPath $ResultPath -Raw}
if($process.ExitCode -ne 0) {throw "Guest capture controller failed with exit $($process.ExitCode). See result and stdout/stderr."}
