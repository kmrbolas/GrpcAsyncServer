param([string] $file, [string] $lang = "cpp", [bool] $nogrpc = $false, [string] $outputDir)

$includePath = "C:\vcpkg\installed\x64-windows-static\include"
$pluginPath = "C:\vcpkg\installed\x64-windows-static\tools\grpc\"

function FileExists([string] $file) { return Test-Path $file -PathType Leaf }

function CopyIfDiffer([string] $file, [string] $dst) {
    $h1 = (Get-FileHash $file -Algorithm MD5 | Select-Object).Hash
    $dstFile = (Join-Path $dst $file);
    $h2 = if (Test-Path -Path $dstFile) { (Get-FileHash $dstFile -Algorithm MD5 | Select-Object).Hash }
    else { $null }
    if ($h1 -ne $h2)
    {
        Copy-Item -Path $file -Destination $dst -Force
        Write-Host "File $($file) copied to the destination $($dst)."
    }
    else { Write-Host "Skipping copy of $($file) to the destination $($dstFile), both file hashes match." }
}

$ffile = [System.IO.Path]::GetFileNameWithoutExtension($file)

[string[]] $outputFiles = @()
if ($lang -eq "cpp")
{
    if (!$nogrpc)
    {
        $outputFiles += "$($ffile).grpc.pb.h"
        $outputFiles += "$($ffile).grpc.pb.cc"
    }
    $outputFiles += "$($ffile).pb.h"
    $outputFiles += "$($ffile).pb.cc"
}
elseif ($lang -eq "csharp")
{
    $ffile = $ffile -replace "_", ""
    $outputFiles += "$($ffile).cs"
    $outputFiles += "$($ffile)Grpc.cs"
}

function TestGeneratedFiles([string]$file) {
    [string[]] $arr = @();
    for ($i = if ($lang -eq "cpp" -and $nogrpc) { 2 } else { 0 }; $i -lt $outputFiles.Count; $i++) {
        if (!(FileExists($outputFiles[$i])))
        {   $arr += $outputFiles[$i];    }
    }
    return $arr;
}

if ($null -eq $file -or $file.Length -eq 0)
{
    Write-Host "Use the paramter -file with the name of the archive to compile."
    exit -1
}

if (!(FileExists($file)) -and !(FileExists("$($file).proto")))
{
    Write-Host "File $($file) not found."
    exit -2
}

$file = [System.IO.Path]::ChangeExtension($file, ".proto")

$hash = (Get-FileHash $file -Algorithm MD5 | Select-Object).Hash
$hashPath = "$($file).hash"

if (!(Test-Path $hashPath -PathType Leaf))
{
    $savedHash = $null
    Write-Host "Hash file not found."
}
elseif (($arr = TestGeneratedFiles($file)).Length -ne 0)
{
    Write-Host "Files not found:"
    foreach ($f in $arr)
    {
        Write-Host $f
    }
    $savedHash = $null
}
else
{
    $savedHash = (Get-Item -Path $hashPath) | Get-Content -First 1
}

if (!($savedHash -eq $hash))
{
    if ($nogrpc) { protoc $file --grpc_out=. "--$($lang)_out=." --proto_path=. --proto_path=$includePath }
    else { protoc $file --grpc_out=. "--$($lang)_out=." --plugin=protoc-gen-grpc="$($pluginPath)grpc_$($lang)_plugin.exe"  --proto_path=. --proto_path=$includePath }
    
    if ($LastExitCode -eq 0)
    {
        $hash > $hashPath
        Write-Host "Files generated successfully"
    }
    else
    {
        Write-Host "Error while executing protoc, exit code: $($LastExitCode)."
    }
}
else
{
    Write-Host "The files are already updated."
    $LastExitCode = 0
}

if (($outputDir.Length -ne 0) -and (Test-Path $outputDir))
{
    foreach ($f in $outputFiles)
    {
        CopyIfDiffer -file $f -dst $outputDir
    }
}

exit $LastExitCode