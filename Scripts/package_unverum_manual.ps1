# Creates Unverum package

$Project_Dir = (get-item $PSScriptRoot).Parent.FullName
$Package_Dir = "$Project_Dir\Packages\Unverum_Manual"
$Build_Dir = "$Project_Dir\build"
$Mod_DLL = "$Build_Dir\StriveReplayRewind\Game__Shipping__Win64\StriveReplayRewind.dll"
$UE4SS_DLL = "$Build_Dir\Output\Game__Shipping__Win64\UE4SS\bin\UE4SS.dll"
$Proxy_DLL = "$Build_Dir\Output\Game__Shipping__Win64\proxy\bin\dwmapi.dll"
$Installation_Instructions = "$Project_Dir\prereqs\Unverum Installation Instructions.txt"


if(-Not (Test-Path -Path "$Mod_DLL")) {
    Write-Host "Failed to find Mod DLL"
    Exit 1
}
if(-Not (Test-Path -Path "$UE4SS_DLL")) {
    Write-Host "Failed to find UE4SS DLL"
    Exit 1
}
if(-Not (Test-Path -Path "$Proxy_DLL")) {
    Write-Host "Failed to find Proxy DLL"
    Exit 1
}
if(-Not (Test-Path -Path "$Installation_Instructions")) {
    Write-Host "Failed to find Unverum Installation Instructions"
    Exit 1
}


Write-Host "Cleaning Package Dir"
if(Test-Path -Path "$Package_Dir"){
    Remove-Item "$Package_Dir" -Recurse | Out-Null
}

Write-Host "Creating Package"
New-Item -ItemType Directory "$Package_Dir\Strive Frame Viewer\ue4ss\StriveReplayRewind\dlls" | Out-Null
Copy-Item "$Mod_DLL" "$Package_Dir\Strive Frame Viewer\ue4ss\StriveReplayRewind\dlls\main.dll" | Out-Null

New-Item -ItemType Directory "$Package_Dir\Dependencies\ue4ss" | Out-Null
Copy-Item "$UE4SS_DLL" "$Package_Dir\Dependencies\ue4ss\ue4ss.dll" | Out-Null
Copy-Item "$Proxy_DLL" "$Package_Dir\Dependencies\ue4ss\dwmapi.dll" | Out-Null

Copy-Item "$Installation_Instructions" "$Package_Dir\Unverum Installation Instructions.txt" | Out-Null