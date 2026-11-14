# tools/detect.ps1 - what kind of game copy a path is and its PARAM.SFO title. Setup uses it to find
# the player's game (a folder, an ISO or a PKG) and the files the decryptor step is given.
#
# Dot-source it (it only defines functions):
#     . (Join-Path $PSScriptRoot "detect.ps1")
#     $game = Get-GameInfo -Path "D:\Games\inFAMOUS 2"
#
# Run it directly to see what it finds:
#     powershell -ExecutionPolicy Bypass -File tools\detect.ps1 "D:\Games\inFAMOUS 2"
#
# Get-GameInfo returns a hashtable:
#     Format     "iso" | "folder" | "pkg"
#     TitleId    TITLE_ID from PARAM.SFO (a PKG: from its content id header, else $null)
#     Serial     the same id as players know it, e.g. BCUS98125, or $null
#     EbootPath  USRDIR\EBOOT.BIN for a folder; for an ISO the same path inside the image
#                (extract that entry first); $null for a PKG

$script:DetectDir = $PSScriptRoot

# PARAM.SFO string values: UTF-8 (with or without a BOM) or UTF-16-LE. PSF stores the byte
# count, so a BOM-less UTF-16 value is recognised by its zero high bytes.
function ConvertFrom-SfoString {
    param([byte[]]$Bytes, [int]$Offset, [int]$Length)
    if ($Length -le 0) { return "" }

    if ($Length -ge 2 -and $Bytes[$Offset] -eq 0xFF -and $Bytes[$Offset + 1] -eq 0xFE) {
        return [Text.Encoding]::Unicode.GetString($Bytes, $Offset + 2, $Length - 2).TrimEnd([char]0)
    }
    if ($Length -ge 2 -and $Bytes[$Offset] -eq 0xFE -and $Bytes[$Offset + 1] -eq 0xFF) {
        return [Text.Encoding]::BigEndianUnicode.GetString($Bytes, $Offset + 2, $Length - 2).TrimEnd([char]0)
    }
    if ($Length -ge 4 -and ($Length % 2) -eq 0) {
        $odd = [int]($Length / 2)
        $zeros = 0
        for ($i = $Offset + 1; $i -lt $Offset + $Length; $i += 2) {
            if ($Bytes[$i] -eq 0) { $zeros++ }
        }
        if ($zeros -ge [Math]::Ceiling($odd * 0.6)) {
            return [Text.Encoding]::Unicode.GetString($Bytes, $Offset, $Length).TrimEnd([char]0)
        }
    }
    return [Text.Encoding]::UTF8.GetString($Bytes, $Offset, $Length).TrimEnd([char]0)
}

# PARAM.SFO -> hashtable. Header: magic "\0PSF", key table, data table and the entry count
# (psdevwiki, PARAM.SFO): every 16-byte entry names a key and a value (0x0404 = uint32,
# 0x0204/0x0004 = string).
function ConvertFrom-ParamSfoBytes {
    param([byte[]]$Bytes, [string]$Source = "PARAM.SFO")
    if ($null -eq $Bytes -or $Bytes.Length -lt 20 -or
        $Bytes[0] -ne 0 -or $Bytes[1] -ne 0x50 -or $Bytes[2] -ne 0x53 -or $Bytes[3] -ne 0x46) {
        throw "$Source is not a PARAM.SFO"
    }

    $keyTable  = [BitConverter]::ToUInt32($Bytes, 8)
    $dataTable = [BitConverter]::ToUInt32($Bytes, 12)
    $count     = [BitConverter]::ToUInt32($Bytes, 16)
    $out = @{}

    for ($i = 0; $i -lt $count; $i++) {
        $e = 20 + 16 * $i
        if ($e + 16 -gt $Bytes.Length) { break }

        $keyOff  = [BitConverter]::ToUInt16($Bytes, $e)
        $format  = [BitConverter]::ToUInt16($Bytes, $e + 2)
        $length  = [BitConverter]::ToUInt32($Bytes, $e + 4)
        $dataOff = [BitConverter]::ToUInt32($Bytes, $e + 12)

        $k = $keyTable + $keyOff
        $end = $k
        while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) { $end++ }
        if ($k -gt $Bytes.Length) { continue }
        $name = [Text.Encoding]::ASCII.GetString($Bytes, $k, $end - $k)

        $vStart = $dataTable + $dataOff
        if ($vStart -gt $Bytes.Length) { continue }
        if ($format -eq 0x0404) {
            if ($vStart + 4 -le $Bytes.Length) { $out[$name] = [BitConverter]::ToUInt32($Bytes, $vStart) }
        } else {
            $len = [int]$length
            if ($len -gt $Bytes.Length - $vStart) { $len = $Bytes.Length - $vStart }
            $out[$name] = ConvertFrom-SfoString -Bytes $Bytes -Offset $vStart -Length $len
        }
    }
    return $out
}

function Read-ParamSfo {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    ConvertFrom-ParamSfoBytes -Bytes ([IO.File]::ReadAllBytes($full)) -Source $Path
}

# --- ISO 9660 (what PS3 .iso dumps carry) -------------------------------------------------------
# Enough of the format to walk the root to PS3_GAME\PARAM.SFO: volume descriptor, directory
# records, one file. A UDF-only image simply yields $null (the format stays "iso").
function Read-IsoRange {
    param([IO.FileStream]$Stream, [long]$Offset, [int]$Length)
    if ($Length -le 0) { return ,(New-Object byte[] 0) }
    $buf = New-Object byte[] $Length
    $Stream.Position = $Offset
    $read = 0
    while ($read -lt $Length) {
        $n = $Stream.Read($buf, $read, $Length - $read)
        if ($n -le 0) { break }
        $read += $n
    }
    if ($read -lt $Length) { return $null }
    return ,$buf
}

function Read-IsoSector {
    param([IO.FileStream]$Stream, [long]$Lba)
    Read-IsoRange -Stream $Stream -Offset ($Lba * 2048) -Length 2048
}

function Find-IsoEntry {
    param([IO.FileStream]$Stream, [byte[]]$DirectoryRecord, [string]$Name)
    if ($DirectoryRecord.Length -lt 34) { return $null }
    $lba  = [BitConverter]::ToUInt32($DirectoryRecord, 2)
    $size = [BitConverter]::ToUInt32($DirectoryRecord, 10)
    if ($size -le 0 -or $size -gt 16MB) { return $null }

    $data = Read-IsoRange -Stream $Stream -Offset ([long]$lba * 2048) -Length ([int]$size)
    if (-not $data) { return $null }

    $pos = 0
    while ($pos + 34 -le $data.Length) {
        $len = [int]$data[$pos]
        if ($len -eq 0) {
            # records never cross a sector: skip to the next one
            $pos = ([int]($pos / 2048) + 1) * 2048
            continue
        }
        if ($pos + $len -gt $data.Length) { break }

        $nameLen = [int]$data[$pos + 32]
        if ($nameLen -gt 0 -and $pos + 33 + $nameLen -le $data.Length) {
            if (-not ($nameLen -eq 1 -and $data[$pos + 33] -le 1)) {
                $nm = [Text.Encoding]::ASCII.GetString($data, $pos + 33, $nameLen)
                $nm = ($nm -split ';')[0]
                if ($nm -ieq $Name) {
                    $rec = New-Object byte[] $len
                    [Array]::Copy($data, $pos, $rec, 0, $len)
                    return ,$rec
                }
            }
        }
        $pos += $len
    }
    return $null
}

function Read-IsoFile {
    param([IO.FileStream]$Stream, [byte[]]$Record)
    if ($Record.Length -lt 34) { return $null }
    $lba  = [BitConverter]::ToUInt32($Record, 2)
    $size = [BitConverter]::ToUInt32($Record, 10)
    if ($size -le 0 -or $size -gt 16MB) { return $null }
    Read-IsoRange -Stream $Stream -Offset ([long]$lba * 2048) -Length ([int]$size)
}

function Get-IsoParamSfo {
    param([string]$Path)
    $fs = $null
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)

        $pvd = Read-IsoSector -Stream $fs -Lba 16
        if (-not $pvd -or $pvd.Length -lt 190 -or $pvd[0] -ne 1 -or
            [Text.Encoding]::ASCII.GetString($pvd, 1, 5) -ne "CD001") {
            return $null
        }
        $root = New-Object byte[] 34
        [Array]::Copy($pvd, 156, $root, 0, 34)

        $ps3 = Find-IsoEntry -Stream $fs -DirectoryRecord $root -Name "PS3_GAME"
        if (-not $ps3) { return $null }
        $sfo = Find-IsoEntry -Stream $fs -DirectoryRecord $ps3 -Name "PARAM.SFO"
        if (-not $sfo) { return $null }

        $bytes = Read-IsoFile -Stream $fs -Record $sfo
        if (-not $bytes) { return $null }
        ConvertFrom-ParamSfoBytes -Bytes $bytes -Source "$Path (PS3_GAME/PARAM.SFO)"
    } catch {
        return $null
    } finally {
        if ($fs) { $fs.Dispose() }
    }
}

# --- PKG ----------------------------------------------------------------------------------------
# A PS3 PKG header (0xA0 bytes) carries the content id at 0x30, e.g.
# "UP0001-BCUS98125_00-...". Older/side-loaded packages may not: then $null.
function Get-PkgTitleId {
    param([string]$Path)
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $h = New-Object byte[] 0xA0
            $n = $fs.Read($h, 0, $h.Length)
            if ($n -lt 4 -or $h[0] -ne 0x7F -or $h[1] -ne 0x50 -or
                $h[2] -ne 0x4B -or $h[3] -ne 0x47) {
                return $null
            }
            if ($n -ge 0x60) {
                $contentId = [Text.Encoding]::ASCII.GetString($h, 0x30, 0x30)
                $m = [regex]::Match($contentId, '[A-Z]{4}[0-9]{5}')
                if ($m.Success) { return $m.Value }
            }
            $header = [Text.Encoding]::ASCII.GetString($h, 0, $n)
            $m = [regex]::Match($header, '[A-Z]{4}[0-9]{5}')
            if ($m.Success) { return $m.Value }
            return $null
        } finally {
            $fs.Dispose()
        }
    } catch {
        return $null
    }
}

# --- the public functions -----------------------------------------------------------------------
function Get-GameInfo {
    param([Parameter(Mandatory = $true)][string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full)) {
        throw "no such file or folder: $Path"
    }
    $item = Get-Item -LiteralPath $full -Force
    $full = $item.FullName

    if ($item.PSIsContainer) {
        $gameDir = $null
        if (Test-Path -LiteralPath (Join-Path $full "PS3_GAME\PARAM.SFO")) {
            $gameDir = Join-Path $full "PS3_GAME"
        } elseif ((Split-Path -Leaf $full) -ieq "PS3_GAME" -and
                  (Test-Path -LiteralPath (Join-Path $full "PARAM.SFO"))) {
            $gameDir = $full
        } else {
            throw "not a PS3 game folder: $Path has no PS3_GAME\PARAM.SFO (choose the folder that holds PS3_GAME, or PS3_GAME itself)"
        }
        $sfo = Read-ParamSfo (Join-Path $gameDir "PARAM.SFO")
        $titleId = $null
        if ($sfo.ContainsKey("TITLE_ID")) { $titleId = [string]$sfo["TITLE_ID"] }
        return @{
            Format    = "folder"
            TitleId   = $titleId
            Serial    = $titleId
            EbootPath = (Join-Path $gameDir "USRDIR\EBOOT.BIN")
        }
    }

    $ext = [IO.Path]::GetExtension($full).ToLowerInvariant()
    if ($ext -eq ".iso") {
        $titleId = $null
        $sfo = Get-IsoParamSfo $full
        if ($sfo -and $sfo.ContainsKey("TITLE_ID")) { $titleId = [string]$sfo["TITLE_ID"] }
        return @{
            Format = "iso"
            TitleId = $titleId
            Serial = $titleId
            # One file, so this is the EBOOT's path inside the image: extract the entry first
            # (7z x game.iso PS3_GAME/USRDIR/EBOOT.BIN) and decrypt that.
            EbootPath = (Join-Path $full "PS3_GAME\USRDIR\EBOOT.BIN")
        }
    }
    if ($ext -eq ".pkg") {
        $titleId = Get-PkgTitleId $full
        return @{
            Format = "pkg"
            TitleId = $titleId
            Serial = $titleId
            EbootPath = $null
        }
    }

    throw "not a game: expected an .iso, a .pkg, or a folder that holds PS3_GAME (got $Path)"
}

# Direct run: print the detection as JSON.
if ($MyInvocation.InvocationName -ne '.') {
    if ($args.Count -lt 1 -or -not $args[0]) {
        [Console]::Error.WriteLine("usage: detect.ps1 <game folder | game.iso | game.pkg>")
        exit 2
    }
    try {
        $g = Get-GameInfo -Path ([string]$args[0])
        $g | ConvertTo-Json -Depth 3
    } catch {
        [Console]::Error.WriteLine("detect.ps1: $($_.Exception.Message)")
        exit 1
    }
}
