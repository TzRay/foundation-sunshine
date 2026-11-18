param(
 [Parameter(Mandatory=$true)][string]$Config,
 [Parameter(Mandatory=$true)][string]$Build,
 [Parameter(Mandatory=$true)][string]$Python,
 [Parameter(Mandatory=$true)][string]$Output,
 [ValidateRange(5,1800)][int]$Seconds=60,
 [switch]$SourceTones
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh managed verification directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$stateFile=Join-Path $env:ProgramData 'Sunshine/MAT/manager.json'
$original=[IO.File]::ReadAllBytes($Config)
function Set-Setting([string]$Key,[string]$Value){
 $text=[IO.File]::ReadAllText($Config,[Text.Encoding]::UTF8)
 $replacement=$Key+' = '+$Value
 $text=[regex]::Replace($text,"(?m)^\s*$Key\s*=.*$",[Text.RegularExpressions.MatchEvaluator]{param($m) $replacement})
 [IO.File]::WriteAllText($Config,$text,[Text.UTF8Encoding]::new($false))
}
function Wait-Manager([string]$State){
 $deadline=[DateTime]::UtcNow.AddSeconds(30)
 do {
  Start-Sleep -Milliseconds 200
  $value=Get-Content -LiteralPath $stateFile -Raw -Encoding utf8 | ConvertFrom-Json
  if($value.state -eq $State -and [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()-$value.updated_ms -lt 5000){return $value}
 } until([DateTime]::UtcNow -ge $deadline)
 throw "Manager did not reach $State : $($value | ConvertTo-Json -Compress)"
}
function Stop-Source {
 Set-Setting 'mat_live' 'disabled'
 $stopped=Wait-Manager 'disabled'
 if($stopped.publisher_pid){throw 'Disabled manager retained its publisher'}
}
function Start-Source {
 Set-Setting 'mat_live' 'enabled'
 return Wait-Manager 'ready'
}
function Verify-Pipe($Manager,[string]$Name,[int]$Duration){
 $result=Join-Path $out ($Name+'.json')
 $checker=(Join-Path $PSScriptRoot '../../tests/mat-live/bed_pipe_verify.py')
 $arguments='"'+$checker+'" --pipe "\\.\pipe\SunshineMatLive-active" --capture-log "'+$Manager.log+'" --output "'+$result+'"'
 if($SourceTones){$arguments+=' --tone-probe "'+(Join-Path $Build 'bed-source-probe.exe')+'" --endpoint "'+$Manager.endpoint+'"'}
 $reader=Start-Process -FilePath $Python -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out ($Name+'.stdout.log')) -RedirectStandardError (Join-Path $out ($Name+'.stderr.log'))
 $deadline=[DateTime]::UtcNow.AddSeconds($Duration)
 do {
  Start-Sleep -Milliseconds 500
  if($reader.HasExited){throw "Pipe reader exited early; inspect $Name.stderr.log"}
 } until([DateTime]::UtcNow -ge $deadline)
 Stop-Source
 if(!$reader.WaitForExit(10000) -or $reader.ExitCode -ne 0){throw "Pipe receiver did not complete cleanly: $Name"}
 $proof=Get-Content -LiteralPath $result -Raw -Encoding utf8 | ConvertFrom-Json
 $capture=Get-Content -LiteralPath $Manager.log -Raw -Encoding utf8
 if($proof.result -ne 'PASS' -or $proof.cycles -lt 40*$Duration -or
    $capture -notmatch ('LEASE_END_ACK cycles='+$proof.cycles+' sha256='+$proof.sha256) -or
    $capture -notmatch 'BED_STOP verified=1'){throw "Native/receiver digest or clean stop mismatch: $Name"}
 return $proof
}
try {
 $first=Wait-Manager 'ready'
 $short=Verify-Pipe $first 'initial' 5
 $second=Start-Source
 if($second.publisher_created -eq $first.publisher_created){throw 'MAT restart reused the old publisher'}
 Set-Setting 'audio_sink' ('Sunshine-MAT-missing-'+[guid]::NewGuid().ToString('N'))
 $missing=Wait-Manager 'error'
 if($missing.publisher_pid){throw 'Missing endpoint retained its publisher'}
 [IO.File]::WriteAllBytes($Config,$original)
 $recovered=Wait-Manager 'ready'
 $soak=Verify-Pipe $recovered 'continuous' $Seconds
 $final=Start-Source
 @{result='PASS';source='bed';initial=$short;continuous=$soak;continuousSeconds=$Seconds;
   restartVerified=$true;endpointLossRecoveryVerified=$true;finalManager=$final;
   shieldConnected=$false;hardwarePlaybackVerified=$false;avSyncVerified=$false} |
  ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $out 'result.json') -Encoding utf8
} finally {
 [IO.File]::WriteAllBytes($Config,$original)
}
