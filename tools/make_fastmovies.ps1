# Test-only disc + hdd0 views where every pre-rendered cutscene movie (movies\cutscene\*.bik) is
# the developers' own 4 s placeholder (cut_scene_place_holder.bik, shipped on the disc), so a test
# does not sit through the 70 s New Marais intro comic or the 96 s opening montage (Sami: "skip
# the intro, it's just a video"). Everything else is the real game: directory junctions and file
# hard links into game\disc and game\dev_hdd0, nothing copied, nothing changed in the originals.
#   powershell -File tools\make_fastmovies.ps1          -> work\fastmovies\disc, work\fastmovies\dev_hdd0
# Used by tools\fast.ps1 ('#! fastmovies 1' in a steps file): PS3_VFS_ROOT / PS3_HDD0_ROOT point here.
# In-world TV feeds (movies\tv) stay real. Re-run after the game folders change (it rebuilds).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$game = Join-Path $root "game"
$out = Join-Path $root "work\fastmovies"
$ph = Join-Path $game "disc\PS3_GAME\USRDIR\movies\cutscene\cut_scene_place_holder.bik"
if (-not (Test-Path $ph)) { throw "no placeholder movie at $ph" }
if (Test-Path $out) { cmd /c rmdir /s /q "$out" | Out-Null }   # junctions are removed, never their targets

# mirror $src into $dst: junction every directory except the one on $keepPath (a relative path we
# descend into), hard link every file
function Mirror([string]$src, [string]$dst, [string[]]$descend) {
    New-Item -ItemType Directory -Force $dst | Out-Null
    foreach ($e in Get-ChildItem -Force $src) {
        $t = Join-Path $dst $e.Name
        if ($e.PSIsContainer) {
            if ($descend.Count -gt 0 -and $e.Name -eq $descend[0]) {
                Mirror $e.FullName $t ($descend | Select-Object -Skip 1)
            } else { New-Item -ItemType Junction -Path $t -Target $e.FullName | Out-Null }
        } elseif ($descend.Count -eq 0 -and $e.Extension -eq ".bik" -and $e.Name -ne "cut_scene_place_holder.bik") {
            New-Item -ItemType HardLink -Path $t -Target $ph | Out-Null          # the placeholder under the movie's name
        } else { New-Item -ItemType HardLink -Path $t -Target $e.FullName | Out-Null }
    }
}
Mirror (Join-Path $game "disc") (Join-Path $out "disc") @("PS3_GAME", "USRDIR", "movies", "cutscene")
# the HDD install by its own title (BCES01143_INSTALL on the EU disc; another name on other copies)
$inst = @(Get-ChildItem (Join-Path $game "dev_hdd0\game") -Directory -Filter "*_INSTALL" -ErrorAction SilentlyContinue | Sort-Object Name)
$instName = if ($inst.Count) { $inst[0].Name } else { "BCES01143_INSTALL" }
Mirror (Join-Path $game "dev_hdd0") (Join-Path $out "dev_hdd0") @("game", $instName, "USRDIR", "movies", "cutscene")
$n = (Get-ChildItem "$out\disc\PS3_GAME\USRDIR\movies\cutscene\*.bik").Count + (Get-ChildItem "$out\dev_hdd0\game\$instName\USRDIR\movies\cutscene\*.bik" -ErrorAction SilentlyContinue).Count
"fast-movie views in ${out}: $n cutscene movies -> the 4 s placeholder"
