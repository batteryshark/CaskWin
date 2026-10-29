# Build the portable part of Wine's version implementation from the upstream
# source on every build. Keep copyright notices and fail if source markers move.
param([Alias('Repo')][string]$WineSourceDirectory,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'wine-source.ps1')
$WineSourceDirectory=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$nt=Get-Content -Raw -LiteralPath (Join-Path $WineSourceDirectory 'dlls\ntdll\version.c')
$kb=Get-Content -Raw -LiteralPath (Join-Path $WineSourceDirectory 'dlls\kernelbase\version.c')
function Extract-Function([string]$source,[string]$name) {
    $match=[regex]::Match($source,'(?m)^(?:static inline )?\w+\s+(?:WINAPI )?'+$name+'\s*\(')
    if(!$match.Success){throw "Wine version function not found: $name"}
    $start=$match.Index
    $brace=$source.IndexOf('{',$start)
    $depth=1; $end=$brace+1
    while($depth -and $end -lt $source.Length){
        if($source[$end] -eq '{'){$depth++}
        if($source[$end] -eq '}'){$depth--}
        $end++
    }
    if($depth){throw "Unclosed Wine version function: $name"}
    return $source.Substring($start,$end-$start)+"`n"
}
$tableStart=$nt.IndexOf('typedef enum')
$tableEnd=$nt.IndexOf('static char wine_version')
if($tableStart -lt 0 -or $tableEnd -le $tableStart){throw 'Wine version table markers changed'}
$text=$nt.Substring(0,$nt.IndexOf('*/')+2)+"`n"+$kb.Substring(0,$kb.IndexOf('*/')+2)+@'

/* Generated from Wine ntdll/version.c and kernelbase/version.c.
 * Windows adapter supplies registry selection before application startup.
 * Only kernelbase's lazy, manifest-aware init is omitted: these functions
 * are used only for an explicit Wine Version setting. */
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
namespace wine_version {
BOOL WINAPI wine_GetVersionExW(OSVERSIONINFOW *);

'@+$nt.Substring($tableStart,$tableEnd-$tableStart)
foreach($name in @('RtlGetVersion','RtlGetNtVersionNumbers','version_update_condition','version_compare_values','RtlVerifyVersionInfo')){
    $text+=Extract-Function $nt $name
}
foreach($name in @('GetVersion','GetVersionExA','GetVersionExW')){
    $body=Extract-Function $kb $name
    if($name -eq 'GetVersionExW'){
        $body=$body.Replace('    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;','')
        $body=$body.Replace('    if (!InitOnceExecuteOnce(&init_once, init_current_version, NULL, NULL)) return FALSE;','')
        $body=$body.Replace('current_version.','current_version->')
    }
    $text+=$body
}
foreach($name in @('RtlGetVersion','RtlGetNtVersionNumbers','RtlVerifyVersionInfo','GetVersionExW','GetVersionExA','GetVersion')){
    $text=[regex]::Replace($text,'\b'+$name+'\s*\(','wine_'+$name+'(')
}
$text+="`n} // namespace wine_version`n#undef TRACE`n#undef WARN`n"
[IO.File]::WriteAllText($Output,$text)
