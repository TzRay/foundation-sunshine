param(
 [Parameter(Mandatory=$true)][string]$Build,
 [Parameter(Mandatory=$true)][string]$Output,
 [Parameter(Mandatory=$true)][uint32]$EnginePid,
 [Parameter(Mandatory=$true)][uint64]$EngineCreated,
 [switch]$Reattach
)
# Explicit, bounded native diagnostic. Run elevated; no automatic elevation,
# audio-service restart, endpoint edits, object hooks, or host deployment.
$ErrorActionPreference='Stop'
$elevated=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if(!$elevated){throw 'Native bed verification requires an administrator PowerShell; no injection attempted'}
$engine=Get-Process -Id $EnginePid
if($engine.ProcessName -ne 'audiodg' -or $engine.StartTime.ToFileTimeUtc().ToString() -ne $EngineCreated.ToString()){throw 'Exact audiodg PID/creation identity required'}
$root=(Resolve-Path -LiteralPath $Build).Path
$probe=Join-Path $root 'bed-probe.exe'
$worker=Join-Path $root 'sunshine-sonic-mat-worker.dll'
$probeHash=(Get-FileHash -LiteralPath $probe).Hash.ToLower()
$workerHash=(Get-FileHash -LiteralPath $worker).Hash.ToLower()
if(Test-Path -LiteralPath $Output){throw 'Fresh native bed verification output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$results=@()
$report=[ordered]@{
 schema='mat-bed-m0-v1';enginePid=$EnginePid;engineCreated=$EngineCreated.ToString();
 probeSha256=$probeHash;workerSha256=$workerHash;nativeEncoderAccepted=$false;
 speakerMappingVerified=$false;hardwarePlaybackVerified=$false;cases=@();failure=$null
}
try{
 $reuse=[bool]$Reattach
 foreach($mode in @('full','subset')){
  foreach($layout in @('2.0','5.1','7.1')){
   $log=Join-Path $out "$mode-$layout.log"
   $arguments=@('--engine-pid',"$EnginePid",'--engine-created',"$EngineCreated",'--codec',$worker,
    '--codec-sha256',$workerHash,'--mask',$layout,'--bed',$mode,'--seconds','1')
   if($reuse){$arguments+='--reattach'}
   & $probe @arguments 2>&1 | Out-File -LiteralPath $log -Encoding utf8
   $code=$LASTEXITCODE
   $text=Get-Content -Raw -LiteralPath $log -Encoding UTF8
   $passed=$code -eq 0 -and $text -match '(?m)^BED_PROBE_PASS .*nativeEncoderAccepted=1 carrierValidated=1 .*hardwarePlaybackVerified=0'
   $results+=@{bed=$mode;layout=$layout;exitCode=$code;passed=[bool]$passed;log=$log}
   $report.cases=@($results)
   if(!$passed){throw "Native bed case failed: $mode / $layout; inspect $log"}
   $reuse=$true
  }
 }
 $report.nativeEncoderAccepted=$true
}catch{
 $report.failure=$_.Exception.Message
 throw
}finally{
 $report|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $out 'report.json') -Encoding utf8
}
$report|ConvertTo-Json -Depth 5
