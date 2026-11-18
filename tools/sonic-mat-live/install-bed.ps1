param(
 [Parameter(Mandatory=$true)][string]$Build,
 [string]$HostPackage,
 [string]$ExpectedHostSha256,
 [string]$AudioSink,
 [string]$Python,
 [ValidateRange(5,1800)][int]$VerifySeconds=60,
 [switch]$SourceTones,
 [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Run this installer as administrator'}
if(Test-Path -LiteralPath $Output){throw 'Fresh installation evidence directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
Start-Transcript -LiteralPath (Join-Path $out 'install.log') | Out-Null
$install=Join-Path $env:ProgramFiles 'Sunshine'
$config=Join-Path $install 'config/sunshine.conf'
$capture=Join-Path (Resolve-Path -LiteralPath $Build).Path 'sunshine-plugin-mat-capture.exe'
$serviceWasRunning=(Get-Service SunshineService).Status -eq 'Running'
function Stop-Sunshine {
 Stop-Service SunshineService
 (Get-Service SunshineService).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(20))
 $deadline=[DateTime]::UtcNow.AddSeconds(20)
 while(Get-Process Sunshine -ErrorAction SilentlyContinue){
  if([DateTime]::UtcNow -ge $deadline){throw 'Sunshine has not exited normally; runtime left unchanged'}
  Start-Sleep -Milliseconds 200
 }
}
try {
 if($HostPackage){
  $source=(Resolve-Path -LiteralPath $HostPackage).Path
  if($ExpectedHostSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
    (Get-FileHash -LiteralPath (Join-Path $source 'sunshine.exe')).Hash -ne $ExpectedHostSha256){throw 'Prepared Sunshine executable hash mismatch'}
 }
 # Keep the original runtime and configuration for an explicit rollback.
 Copy-Item -LiteralPath $config -Destination (Join-Path $out 'sunshine.conf.before')
 if($HostPackage){
  Copy-Item -LiteralPath (Join-Path $install 'sunshine.exe') -Destination (Join-Path $out 'sunshine.exe.before')
  Copy-Item -LiteralPath (Join-Path $install 'assets/web') -Destination (Join-Path $out 'web.before') -Recurse
  Stop-Sunshine
  Copy-Item -LiteralPath (Join-Path $source 'sunshine.exe') -Destination (Join-Path $install 'sunshine.exe') -Force
  foreach($entry in Get-ChildItem -LiteralPath (Join-Path $source 'assets/web')){
   Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $install 'assets/web') -Recurse -Force
  }
 }
 $text=[IO.File]::ReadAllText($config,[Text.Encoding]::UTF8)
 $values=@{mat_live='enabled';mat_source='bed'}
 if($AudioSink){$values.audio_sink=$AudioSink}
 foreach($key in $values.Keys){
  $line=$key+' = '+$values[$key]
  if($text -match "(?m)^\s*$key\s*="){ $text=[regex]::Replace($text,"(?m)^\s*$key\s*=.*$",[Text.RegularExpressions.MatchEvaluator]{param($m) $line}) }
  else{$text=$text.TrimEnd()+"`r`n$line`r`n"}
 }
 [IO.File]::WriteAllText($config,$text,[Text.UTF8Encoding]::new($false))
 & $capture --install-bed $config --log (Join-Path $out 'capture-install.log')
 if($LASTEXITCODE){throw "Native MAT manager installation failed: $LASTEXITCODE"}
 if($serviceWasRunning -and (Get-Service SunshineService).Status -ne 'Running'){Start-Service SunshineService}
 $deadline=[DateTime]::UtcNow.AddSeconds(30)
 do {
  Start-Sleep -Milliseconds 500
  $managerFile=Join-Path $env:ProgramData 'Sunshine/MAT/manager.json'
  if(Test-Path -LiteralPath $managerFile){$manager=Get-Content -LiteralPath $managerFile -Raw -Encoding utf8 | ConvertFrom-Json}
  if($manager.state -eq 'ready'){break}
  if($manager.state -eq 'error'){throw ('MAT capture failed: '+$manager.last_error)}
 } until([DateTime]::UtcNow -ge $deadline)
 if($manager.state -ne 'ready'){throw 'MAT manager did not become ready'}
 if($Python){
  & "$PSScriptRoot/verify-managed-bed.ps1" -Config $config -Build $Build -Python $Python -Output (Join-Path $out 'verification') -Seconds $VerifySeconds -SourceTones:$SourceTones
  if($LASTEXITCODE){throw 'Managed source verification failed'}
  $manager=Get-Content -LiteralPath $managerFile -Raw -Encoding utf8 | ConvertFrom-Json
 }
 $result=@{result='PASS';hostSha256=(Get-FileHash -LiteralPath (Join-Path $install 'sunshine.exe')).Hash.ToLower();
   captureSha256=(Get-FileHash -LiteralPath $capture).Hash.ToLower();task='Sunshine MAT PCM';source='bed';backup=$out;manager=$manager}
 $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'result.json') -Encoding utf8
} catch {
 $_ | Out-String | Set-Content -LiteralPath (Join-Path $out 'error.txt') -Encoding utf8
 if($HostPackage -and (Test-Path -LiteralPath (Join-Path $out 'sunshine.exe.before'))){
  Stop-Sunshine
  Copy-Item -LiteralPath (Join-Path $out 'sunshine.exe.before') -Destination (Join-Path $install 'sunshine.exe') -Force
  foreach($entry in Get-ChildItem -LiteralPath (Join-Path $out 'web.before')){Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $install 'assets/web') -Recurse -Force}
 }
 if(Test-Path -LiteralPath (Join-Path $out 'sunshine.conf.before')){Copy-Item -LiteralPath (Join-Path $out 'sunshine.conf.before') -Destination $config -Force}
 if($serviceWasRunning -and (Get-Service SunshineService).Status -ne 'Running'){Start-Service SunshineService}
 throw
} finally {Stop-Transcript | Out-Null}
