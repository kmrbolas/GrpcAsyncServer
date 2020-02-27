param([string] $file, [string] $lang = "cpp", [switch] $nogrpc, [string] $outputDir)

if ($Env:OS -eq "Windows_NT")
{
    $protoc = "C:\vcpkg\installed\x64-windows-static\tools\protobuf\protoc.exe"
    $includePath = "C:\vcpkg\installed\x64-windows-static\include\"
    $pluginPath = "C:\vcpkg\installed\x64-windows-static\tools\grpc\"
}
else
{
    $protoc = "/opt/special32/protoc"
    $includePath = "/opt/include32/"
    $pluginPath = "/opt/special32/"
}

function FileExists([string] $file) { return Test-Path $file -PathType Leaf }

function FileEqual([string] $f1, [string] $f2)
{
	return (FileExists $f2) -and ((Get-Item $f1).Length -eq (Get-Item $f2).Length) -and ((Get-FileHash $f1 -Algorithm SHA1 | Select-Object).Hash -eq (Get-FileHash $f2 -Algorithm SHA1 | Select-Object).Hash)
}

function CopyIfDiffer([string] $file, [string] $dst)
{
    $dstFile = (Join-Path $dst $file);
    if (FileEqual (Join-Path $PSScriptRoot $file) $dstFile)
    {
        Write-Host "Skipping copy of $($file) to the destination $($dstFile), both file hashes match."
    }
    else
    {
        Copy-Item -Path $file -Destination $dst -Force
        Write-Host "File $($file) copied to the destination $($dst)."
    }
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

$params = if ($nogrpc) { "$file --$($lang)_out=. --proto_path=. --proto_path=$includePath" }
else { "$file --grpc_out=. --$($lang)_out=. --plugin=protoc-gen-grpc=$($pluginPath)grpc_$($lang)_plugin.exe --proto_path=. --proto_path=$includePath" }

& "$protoc" $params.Split(" ")

if ($LastExitCode -eq 0)
{
    $hash > $hashPath
    Write-Host "Files generated successfully"
}
else
{
    Write-Host "Error while executing protoc, exit code: $($LastExitCode)."
    exit $LastExitCode;
}

if (($outputDir.Length -ne 0) -and (Test-Path $outputDir))
{
    foreach ($f in $outputFiles)
    {
        CopyIfDiffer -file $f -dst $outputDir
    }
}

exit 0