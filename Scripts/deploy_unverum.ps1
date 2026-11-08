# Updates Unverum with mod

# modify to target the your Unverum root folder
$Unverum_Dir = "C:\Users\Andrew\Documents\projects\ggst_modding\tools\Unverum"


$Project_Dir = (get-item $PSScriptRoot).Parent.FullName
$Package_Dir = "$Project_Dir\Packages\Unverum"
$Install_Dir = "$Unverum_Dir\Mods\Guilty Gear -Strive-\StriveReplayRewind"

Write-Host "Building Package"
pwsh -File "$PSScriptRoot/package_unverum.ps1"
if($LastExitCode -ne 0) { Exit $LastExitCode }

Write-Host "Cleaning Install Dir"
if(Test-Path -Path "$Install_Dir"){
  Remove-Item "$Install_Dir" -Recurse | Out-Null
}

Write-Host "Installing Package"
New-Item -ItemType Directory "$Install_Dir" | Out-Null
Copy-Item -Path "$Package_Dir\*" -Destination "$Install_Dir" -Recurse | Out-Null

