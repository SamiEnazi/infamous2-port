# tools/rpcs3-adapter.ps1 - the default decryptor (the contract: <tool> "{in}" "{out}").
# RPCS3 decrypts one SELF per start: "--decrypt USRDIR\EBOOT.BIN" writes EBOOT.elf beside the
# input, and liblv2.sprx -> liblv2.prx. This stages the input in a temporary folder, runs RPCS3
# there, checks the result is an ELF and copies it to {out}; it writes nothing beside the input.
#
# RPCS3 is found in config.local.json ("rpcs3": the folder that holds rpcs3.exe), then on PATH.
# Setup calls it as:
#     powershell -ExecutionPolicy Bypass -File tools\rpcs3-adapter.ps1 "{in}" "{out}"

param(
    [Parameter(Mandatory = $true, Position = 0)][string]$In,
    [Parameter(Mandatory = $true, Position = 1)][string]$Out
)
$ErrorActionPreference = "Stop"

function Find-Rpcs3 {
    $root = Split-Path -Parent $PSScriptRoot
    $cfg = Join-Path $root "config.local.json"
    if (Test-Path -LiteralPath $cfg) {
        try {
            $j = Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json
            if ($j.rpcs3) {
                $p = [string]$j.rpcs3
                if ($p -match '\.exe$') {
                    if (Test-Path -LiteralPath $p) { return $p }
                } else {
                    $p = Join-Path $p "rpcs3.exe"
                    if (Test-Path -LiteralPath $p) { return $p }
                }
            }
        } catch {
            # an unreadable config is not fatal: try PATH
        }
    }
    $c = Get-Command "rpcs3.exe" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $c = Get-Command "rpcs3" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

function Test-Elf([string]$Path) {
    $fs = [IO.File]::OpenRead($Path)
    try {
        $sig = New-Object byte[] 4
        $n = $fs.Read($sig, 0, 4)
        return ($n -eq 4 -and $sig[0] -eq 0x7F -and $sig[1] -eq 0x45 -and
                $sig[2] -eq 0x4C -and $sig[3] -eq 0x46)
    } finally {
        $fs.Dispose()
    }
}

try {
    $In = [IO.Path]::GetFullPath($In)
    $Out = [IO.Path]::GetFullPath($Out)
    if (-not (Test-Path -LiteralPath $In)) { throw "no such input: $In" }
    $outDir = Split-Path -Parent $Out
    if (-not (Test-Path -LiteralPath $outDir)) { throw "the output's folder does not exist: $outDir" }

    # Already an ELF (a decryptor earlier in the chain): pass it through.
    if (Test-Elf $In) {
        if (Test-Path -LiteralPath $Out) { [IO.File]::SetAttributes($Out, [IO.FileAttributes]::Normal); Remove-Item -LiteralPath $Out -Force }
        [IO.File]::Copy($In, $Out, $true)
        exit 0
    }

    $exe = Find-Rpcs3
    if (-not $exe) {
        throw ("RPCS3 was not found. Put its folder in config.local.json as `"rpcs3`", or install it, " +
               "or set a different decryptor in the Setup config.")
    }

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("inf2-decrypt-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        # Keep the input's name: RPCS3 names its output after it (EBOOT.BIN -> EBOOT.elf,
        # liblv2.sprx -> liblv2.prx).
        $stage = Join-Path $tmp (Split-Path -Leaf $In)
        [IO.File]::Copy($In, $stage, $true)

        function Get-Produced {
            Get-ChildItem -LiteralPath $tmp -File |
                Where-Object { $_.FullName -ne $stage -and $_.Extension -in @(".elf", ".prx") } |
                Select-Object -First 1
        }

        # First headless (no Qt screen plugin needed), then RPCS3's normal start.
        $produced = $null
        $lastCode = $null
        foreach ($mode in @(@("--headless"), @())) {
            $argList = @($mode) + @("--decrypt", "`"$stage`"")
            $p = Start-Process -FilePath $exe -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden
            $lastCode = $p.ExitCode
            $produced = Get-Produced
            if ($produced) { break }
        }
        if (-not $produced) {
            throw ("RPCS3 did not write a decrypted file (exit $lastCode). Is the PS3 firmware installed in RPCS3? " +
                   "Try File > Install Firmware, then decrypt $In once by hand to check.")
        }
        if (-not (Test-Elf $produced.FullName)) {
            throw "RPCS3 wrote $($produced.Name), but it does not start with an ELF header."
        }

        if (Test-Path -LiteralPath $Out) {
            [IO.File]::SetAttributes($Out, [IO.FileAttributes]::Normal)
            Remove-Item -LiteralPath $Out -Force
        }
        [IO.File]::Copy($produced.FullName, $Out, $true)
        exit 0
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
} catch {
    [Console]::Error.WriteLine("rpcs3-adapter: $($_.Exception.Message)")
    exit 1
}
