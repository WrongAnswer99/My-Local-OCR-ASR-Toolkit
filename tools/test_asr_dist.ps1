# Copied into dist/test_asr.ps1 by make_dist.ps1. No project/Python/ffmpeg runtime.
param([ValidateRange(1,10)][int]$Repeat=1,[string]$ResumeReportDir='')
$ErrorActionPreference='Stop'
$dist=$PSScriptRoot
$env:PATH="$env:SystemRoot\System32;$env:SystemRoot;$PSHOME"
[Environment]::CurrentDirectory=$dist
Set-Location -LiteralPath $dist
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public sealed class AsrDistTest : IDisposable {
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern IntPtr LoadLibraryEx(string path,IntPtr reserved,uint flags);
    [DllImport("kernel32.dll",CharSet=CharSet.Ansi,ExactSpelling=true)]
    static extern IntPtr GetProcAddress(IntPtr library,string name);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr library);
    [StructLayout(LayoutKind.Sequential)]
    public struct Options { public uint size; public int threads; public IntPtr model,language; public int device,reserved; }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void Defaults(ref Options options);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr Create(ref Options options,byte[] error,int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void Release(IntPtr p);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr Run(IntPtr engine,IntPtr path,byte[] error,int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr StringValue(IntPtr p);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr Version();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Count(IntPtr p);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr SegmentText(IntPtr p,int index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long SegmentTime(IntPtr p,int index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double Time(IntPtr p);
    public sealed class Segment { public long start_ms,end_ms; public string text; }
    public sealed class Result {
        public string text,language;
        public double audio_ms,decode_ms,transcribe_ms,sdk_total_ms;
        public List<Segment> segments=new List<Segment>();
    }
    IntPtr library,engine;
    Defaults defaults; Create create; Release destroy,free; Run run;
    StringValue text,language; Count count; SegmentText segmentText;
    SegmentTime start,end; Time audio,decode,transcribe;
    public string sdk_version; public double init_ms;
    T Bind<T>(string name) { IntPtr address=GetProcAddress(library,name); if(address==IntPtr.Zero)throw new Exception("Missing export: "+name);return (T)(object)Marshal.GetDelegateForFunctionPointer(address,typeof(T)); }
    static IntPtr Utf8(string value) { byte[] b=Encoding.UTF8.GetBytes(value+"\0");IntPtr p=Marshal.AllocHGlobal(b.Length);Marshal.Copy(b,0,p,b.Length);return p; }
    static string Read(IntPtr p) { if(p==IntPtr.Zero)return "";int n=0;while(Marshal.ReadByte(p,n)!=0)n++;byte[] b=new byte[n];Marshal.Copy(p,b,0,n);return Encoding.UTF8.GetString(b); }
    static string Error(byte[] b) { int n=Array.IndexOf(b,(byte)0);return Encoding.UTF8.GetString(b,0,n<0?b.Length:n); }
    public AsrDistTest(string dll) {
        library=LoadLibraryEx(dll,IntPtr.Zero,0x100|0x1000);
        if(library==IntPtr.Zero)throw new Exception("Load ASR DLL failed: "+Marshal.GetLastWin32Error());
        try {
            defaults=Bind<Defaults>("asr_default_options");create=Bind<Create>("asr_create");destroy=Bind<Release>("asr_destroy");
            run=Bind<Run>("asr_transcribe_file");free=Bind<Release>("asr_free_result");
            text=Bind<StringValue>("asr_result_text");language=Bind<StringValue>("asr_result_language");count=Bind<Count>("asr_result_segment_count");
            segmentText=Bind<SegmentText>("asr_result_segment_text");start=Bind<SegmentTime>("asr_result_segment_start_ms");end=Bind<SegmentTime>("asr_result_segment_end_ms");
            audio=Bind<Time>("asr_result_audio_ms");decode=Bind<Time>("asr_result_decode_ms");transcribe=Bind<Time>("asr_result_transcribe_ms");
            sdk_version=Read(Bind<Version>("asr_version")());
            Options options=new Options();defaults(ref options);
            if(options.size!=Marshal.SizeOf(typeof(Options)))throw new Exception("Options ABI mismatch");
            byte[] error=new byte[2048];
            options.device=1;
            IntPtr invalid=create(ref options,error,error.Length);
            if(invalid!=IntPtr.Zero){destroy(invalid);throw new Exception("Unsupported device must fail");}
            if(!Error(error).Contains("CPU only"))throw new Exception("Missing CPU-only device error");
            defaults(ref options);options.threads=8;
            Stopwatch timer=Stopwatch.StartNew();engine=create(ref options,error,error.Length);timer.Stop();init_ms=timer.Elapsed.TotalMilliseconds;
            if(engine==IntPtr.Zero)throw new Exception(Error(error));
        } catch { Dispose();throw; }
    }
    public Result Transcribe(string path) {
        IntPtr p=Utf8(path);byte[] error=new byte[2048];IntPtr result=IntPtr.Zero;
        try {
            Stopwatch timer=Stopwatch.StartNew();result=run(engine,p,error,error.Length);timer.Stop();
            if(result==IntPtr.Zero)throw new Exception(Error(error));
            Result value=new Result();value.text=Read(text(result));value.language=Read(language(result));value.audio_ms=audio(result);value.decode_ms=decode(result);value.transcribe_ms=transcribe(result);value.sdk_total_ms=timer.Elapsed.TotalMilliseconds;
            int n=count(result);
            for(int i=0;i<n;i++)value.segments.Add(new Segment {start_ms=start(result,i),end_ms=end(result,i),text=Read(segmentText(result,i))});
            if(Read(segmentText(result,n))!="")throw new Exception("Invalid segment index must return empty");
            return value;
        } finally { if(result!=IntPtr.Zero)free(result);Marshal.FreeHGlobal(p); }
    }
    public void ExpectFailure(string path) { try { Transcribe(path); } catch(Exception) { return; }throw new Exception("Invalid audio must fail: "+path); }
    public void Dispose(){if(engine!=IntPtr.Zero){destroy(engine);engine=IntPtr.Zero;}if(library!=IntPtr.Zero){FreeLibrary(library);library=IntPtr.Zero;}}
}
'@
$stamp=[DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyyMMdd_HHmmss')
$report=Join-Path $dist "asr-test-results\$stamp"
if($ResumeReportDir){
    $report=[IO.Path]::GetFullPath($ResumeReportDir)
    $allowed=[IO.Path]::GetFullPath((Join-Path $dist 'asr-test-results')).TrimEnd('\')+'\'
    if(-not $report.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'ResumeReportDir must be inside dist/asr-test-results'}
}
New-Item -ItemType Directory -Force -Path $report | Out-Null
$savedResults=@()
if($ResumeReportDir -and (Test-Path -LiteralPath (Join-Path $report 'summary.json'))){
    $savedSummary=Get-Content -LiteralPath (Join-Path $report 'summary.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $savedResults=@($savedSummary.results)
}
$results=@();$sdk=$null;$failures=@();$initMs=0;$version=''
$modelHash=(Get-FileHash (Join-Path $dist 'models\whisper\ggml-small-q5_1.bin') -Algorithm SHA256).Hash
$sdkHash=(Get-FileHash (Join-Path $dist 'asr.dll') -Algorithm SHA256).Hash
try {
    $sdk=New-Object AsrDistTest((Join-Path $dist 'asr.dll'))
    $initMs=$sdk.init_ms;$version=$sdk.sdk_version
    if($savedResults.Count -and ($savedSummary.model_sha256 -ne $modelHash -or $savedSummary.sdk_sha256 -ne $sdkHash -or $savedSummary.sdk_version -ne $version)){
        throw 'Saved results use a different SDK/model; run a fresh test'
    }
    $sdk.ExpectFailure((Join-Path $dist 'asr-samples\does-not-exist.wav'))
    $invalid=Join-Path $report 'invalid.wav'
    [IO.File]::WriteAllText($invalid,'not an audio file')
    $sdk.ExpectFailure($invalid)
    # Use Unicode code points so Windows PowerShell 5.1 also reads this
    # UTF-8-without-BOM script correctly on machines with an ANSI code page.
    $chineseStem=-join ([char[]](0x4e2d,0x6587,0x8bed,0x97f3))
    $chineseWord=-join ([char[]](0x6587,0x5b57))
    $expectedChinese=(Get-Content (Join-Path $dist 'asr-samples\expected-chinese.txt') -Raw -Encoding UTF8) -replace '[^\p{L}\p{Nd}]',''
    $cases=@(
        @{file='jfk.wav';language='en';pattern='country';long=$false},
        @{file=($chineseStem+'.wav');language='zh';pattern=$chineseWord;long=$false},
        @{file=($chineseStem+'.mp3');language='zh';pattern=$chineseWord;long=$false},
        @{file=($chineseStem+'.m4a');language='zh';pattern=$chineseWord;long=$false},
        @{file='long-jfk.wav';language='en';pattern='country';long=$true}
    )
    foreach($case in $cases){
        $saved=$savedResults | Where-Object {$_.file -ceq $case.file -and $_.status -eq 'PASS' -and $_.runs_ms.Count -eq $Repeat} | Select-Object -First 1
        if($saved){$results+=$saved;Write-Host "Reusing PASS: $($case.file)";continue}
        $file=Join-Path $dist ('asr-samples\'+$case.file)
        $item=[ordered]@{status='PASS';file=$case.file;error='';runs_ms=@()}
        try {
            $first=$sdk.Transcribe($file)
            if([string]::IsNullOrWhiteSpace($first.text)-or $first.text -notmatch $case.pattern){throw 'Expected words missing from transcription'}
            if($first.language -ne $case.language){throw 'Unexpected detected language'}
            if($case.language -eq 'zh' -and (($first.text -replace '[^\p{L}\p{Nd}]','') -cne $expectedChinese)){throw 'Complete Chinese fixture text mismatch'}
            if($first.segments.Count -eq 0){throw 'No segments returned'}
            foreach($segment in $first.segments){if($segment.start_ms -lt 0 -or $segment.end_ms -lt $segment.start_ms -or $segment.end_ms -gt $first.audio_ms){throw 'Invalid segment timestamp'}}
            if($case.long -and ($first.audio_ms -le 30000 -or $first.segments[$first.segments.Count-1].end_ms -le 30000)){throw 'Audio beyond the first 30-second window was not transcribed'}
            $item.first_sdk_ms=$first.sdk_total_ms;$item.audio_ms=$first.audio_ms;$item.decode_ms=$first.decode_ms;$item.transcribe_ms=$first.transcribe_ms
            $item.language=$first.language;$item.text=$first.text;$item.segments=$first.segments
            for($i=0;$i -lt $Repeat;$i++){
                $again=$sdk.Transcribe($file)
                if($again.text -cne $first.text){throw 'Repeated deterministic transcription changed'}
                $item.runs_ms+= $again.sdk_total_ms
            }
            $item.warm_avg_ms=($item.runs_ms | Measure-Object -Average).Average
            [IO.File]::WriteAllText((Join-Path $report ($case.file+'.txt')),$first.text,(New-Object Text.UTF8Encoding($false)))
            Write-Host ("PASS {0}: audio={1:F2} ms first={2:F2} ms warm={3:F2} ms language={4}" -f $case.file,$item.audio_ms,$item.first_sdk_ms,$item.warm_avg_ms,$item.language)
        } catch {$item.status='FAIL';$item.error=$_.Exception.Message;$failures+=$item.error;Write-Host "FAIL $($case.file): $($item.error)"}
        $results += [pscustomobject]$item
    }
    # Exercise the C++ wrapper/CLI independently from the direct C ABI test.
    $saved=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & (Join-Path $dist 'asr_cli.exe') (Join-Path $dist 'asr-samples\jfk.wav') --threads=8 --language=en ('--json='+ (Join-Path $report 'cli.json')) ('--output='+ (Join-Path $report 'cli.txt')) 2>&1 | Out-File (Join-Path $report 'cli.log') -Encoding UTF8
        $exit=$LASTEXITCODE
    } finally {$ErrorActionPreference=$saved}
    if($exit -ne 0){throw 'ASR CLI failed'}
    $cli=Get-Content (Join-Path $report 'cli.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if($cli.text -notmatch 'country'){throw 'ASR CLI transcript failed validation'}
    $loaded=@([Diagnostics.Process]::GetCurrentProcess().Modules | Where-Object {$_.ModuleName -ieq 'asr.dll'} | ForEach-Object {$_.FileName})
    if($loaded.Count -ne 1 -or $loaded[0] -ine (Join-Path $dist 'asr.dll')){throw 'SDK loaded from outside dist'}
} catch {$failures+=$_.Exception.Message;Write-Host "FAIL: $($_.Exception.Message)"}
finally {if($sdk){$sdk.Dispose()}}
$summary=[ordered]@{tested_at=[DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('o');sdk_version=$version;sdk_sha256=$sdkHash;device='cpu';model='ggml-small-q5_1.bin';model_sha256=$modelHash;init_ms=$initMs;repeats=$Repeat;results=$results;failures=$failures;resumed=[bool]$ResumeReportDir;runtime_scope='Only dist SDK/model/samples and Windows system decoding; no Python, external ffmpeg or API.'}
[IO.File]::WriteAllText((Join-Path $report 'summary.json'),($summary | ConvertTo-Json -Depth 10),(New-Object Text.UTF8Encoding($false)))
$results | Select-Object status,file,language,audio_ms,first_sdk_ms,warm_avg_ms,decode_ms,transcribe_ms | Export-Csv (Join-Path $report 'timings.csv') -NoTypeInformation -Encoding UTF8
$rows=@('# Whisper full-file SDK test','',"SDK: $version",('Initialization: {0:F2} ms; CPU; threads=8; language=auto; repeat={1}' -f $initMs,$Repeat),'','| Status | File | Audio ms | First SDK ms | Warm average ms | Language |','|---|---|---:|---:|---:|---|')
foreach($r in $results){$rows+='| {0} | {1} | {2:F2} | {3:F2} | {4:F2} | {5} |' -f $r.status,$r.file,$r.audio_ms,$r.first_sdk_ms,$r.warm_avg_ms,$r.language}
$rows+=@('','Chinese samples are Windows TTS fixtures, not a recognition-accuracy benchmark. JFK is a public real-speech fixture.','Long audio checks that returned speech timestamps extend beyond 30 seconds.','SDK timings exclude model initialization and managed string decoding/file writes; include native audio decoding/resampling and transcription.','CLI, repeated engine calls, invalid inputs and unsupported device rejection are also checked.','All outputs and runtime inputs are under dist; decoding uses Windows Media Foundation.')
if($failures.Count){$rows+=@('','Failures:');$rows+=$failures}
if($ResumeReportDir){$rows+=@('','This report resumes earlier successful runs with the same SDK/model. Initialization is from the resumed process.')}
[IO.File]::WriteAllText((Join-Path $report 'REPORT.md'),($rows -join "`r`n"),(New-Object Text.UTF8Encoding($false)))
Write-Host "Reports: $report"
if($failures.Count){exit 1}
