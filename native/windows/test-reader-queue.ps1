param([string]$ReaderDirectory=(Join-Path $PSScriptRoot 'test-output\runtime08-readerfix'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\reader-queue-08\fixtures'),[ValidateSet('race','drain','flush','reentrant','eos','deselect')][string[]]$Mode=@('race','drain','flush','reentrant','eos'),[ValidateSet('native','wine')][string[]]$Provider=@('native','wine'),[string]$SelectionMedia='',[switch]$ExpectOldRace,[switch]$SkipBuild)
$ErrorActionPreference='Stop';$ReaderDirectory=[IO.Path]::GetFullPath($ReaderDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-reader-queue-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\reader-queue-08\run-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $suite|Out-Null
$wave=Join-Path $suite 'generated-pcm.wav';$stream=[IO.File]::Create($wave);$writer=[IO.BinaryWriter]::new($stream)
try{$bytes=48000*2*4;$writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'));$writer.Write([uint32]($bytes+36));$writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '));$writer.Write([uint32]16);$writer.Write([uint16]1);$writer.Write([uint16]1);$writer.Write([uint32]48000);$writer.Write([uint32]96000);$writer.Write([uint16]2);$writer.Write([uint16]16);$writer.Write([Text.Encoding]::ASCII.GetBytes('data'));$writer.Write([uint32]$bytes);$writer.Write([byte[]]::new($bytes))}finally{$writer.Dispose();$stream.Dispose()}
$cases=[Collections.Generic.List[object]]::new();$tracked=@{}
if($Mode -contains 'deselect'){
    if(!$SelectionMedia){throw 'The deselect case requires -SelectionMedia pointing to a generated two-stream audiovisual file; it reads only audio while video stays selected.'}
    $SelectionMedia=[IO.Path]::GetFullPath($SelectionMedia);$tracked[$SelectionMedia]=(Get-FileHash -LiteralPath $SelectionMedia).Hash
    Copy-Item -LiteralPath $SelectionMedia -Destination "$suite\selection.mp4"
}
try{foreach($bits in @(32,64)){foreach($providerName in $Provider){
    $arch=if($bits -eq 32){'x86'}else{'x64'};$reader=if($providerName -eq 'native'){'native'}else{"$ReaderDirectory\builtin\$arch\mfreadwrite.dll"}
    $fixture="$FixtureDirectory\reader-queue-probe$bits.exe";$tracked[$fixture]=(Get-FileHash -LiteralPath $fixture).Hash;if($providerName -eq 'wine'){$tracked[$reader]=(Get-FileHash -LiteralPath $reader).Hash}
    foreach($kind in $Mode){
        $name="$providerName-$bits-$kind";$start=[Diagnostics.ProcessStartInfo]::new($fixture);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
        $media=if($kind -eq 'deselect'){"$suite\selection.mp4"}else{$wave}
        foreach($argument in @($reader,$media,$kind)){$start.ArgumentList.Add($argument)}
        $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(25000)){$process.Kill($true);$process.WaitForExit();$code=124}else{$code=$process.ExitCode}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose();[IO.File]::WriteAllText("$suite\$name.log",$text)
        $expectedFailure=$ExpectOldRace -and $providerName -eq 'wine' -and $kind -in @('race','drain','flush','eos')
        $pass=if($expectedFailure){$code -eq 1 -and $text -match 'FAIL (later read cannot steal reserved response|stale ready cannot consume post-flush response)'}else{$code -eq 0 -and $text -match 'PASS deterministic reader queue contract'}
        $cases.Add(@{name=$name;passed=$pass;exitCode=$code;expectedFailure=[bool]$expectedFailure});Write-Host "$name passed=$pass exit=$code expectedFailure=$expectedFailure"
        if(!$pass){Write-Host $text;throw "Reader queue case failed: $name"}
    }
}}
    foreach($file in $tracked.Keys){if((Get-FileHash -LiteralPath $file).Hash -ne $tracked[$file]){throw "Fixture or provider changed: $file"}}
    @{passed=$true;readerDirectory=$ReaderDirectory;cases=$cases;sourceFilesUnchanged=$true;scope='Actual PCM WAV source and optional two-stream selection source with audio requests only; no GPU, audio-device playback or capture; Wine work-item gate uses public callback/state identity and preserves references without private command layout.'}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json";Write-Host "Reader queue results: $suite"
}catch{@{passed=$false;readerDirectory=$ReaderDirectory;cases=$cases;error=$_.Exception.Message}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json";throw}
$global:LASTEXITCODE=0
