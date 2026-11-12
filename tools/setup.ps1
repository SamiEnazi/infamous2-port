# The Setup window (Setup.cmd): pick your game and RPCS3 folders, see what is missing, get the
# missing tools, build the port, play. The work itself is tools\setup_build.ps1.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "setup_common.ps1")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$cfgPath = Join-Path $root "config.local.json"
$saved = @{}
if (Test-Path $cfgPath) { (Get-Content $cfgPath -Raw | ConvertFrom-Json).psobject.Properties | ForEach-Object { $saved[$_.Name] = $_.Value } }
$exe = Join-Path $root "work\port\build\Infamous2Recomp.exe"
$logPath = Join-Path $root "work\setup.log"
$script:proc = $null

$f = New-Object System.Windows.Forms.Form
$f.Text = "inFAMOUS 2 PC port - Setup"
$f.ClientSize = New-Object System.Drawing.Size(760, 640)
$f.StartPosition = "CenterScreen"
$f.Font = New-Object System.Drawing.Font("Segoe UI", 9)
$ico = Join-Path $PSScriptRoot "setup.ico"
if (Test-Path $ico) { $f.Icon = New-Object System.Drawing.Icon $ico }

function New-Label($text, $x, $y, $w, $h, $bold) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object System.Drawing.Point($x, $y); $l.Size = New-Object System.Drawing.Size($w, $h)
    if ($bold) { $l.Font = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold) }
    $f.Controls.Add($l); $l
}
function New-Button($text, $x, $y, $w) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object System.Drawing.Point($x, $y); $b.Size = New-Object System.Drawing.Size($w, 30)
    $f.Controls.Add($b); $b
}
function New-PathRow($label, $y, $value, $hint) {
    New-Label $label 16 $y 728 18 $false | Out-Null
    $t = New-Object System.Windows.Forms.TextBox
    $t.Location = New-Object System.Drawing.Point(16, ($y + 20)); $t.Size = New-Object System.Drawing.Size(620, 24); $t.Text = $value
    $f.Controls.Add($t)
    $b = New-Button "Browse..." 644 ($y + 18) 100
    $b.Tag = @($t, $hint)
    $b.Add_Click({
        $tb = $this.Tag[0]
        $d = New-Object System.Windows.Forms.FolderBrowserDialog
        $d.Description = $this.Tag[1]
        if ($tb.Text -and (Test-Path $tb.Text)) { $d.SelectedPath = $tb.Text }
        if ($d.ShowDialog() -eq "OK") { $tb.Text = $d.SelectedPath; Update-Checks }
    })
    $t
}

New-Label "inFAMOUS 2 - Windows port setup" 16 12 728 26 $true | Out-Null
New-Label ("You need your own copy of the game: the disc's files (the folder with PS3_GAME) and RPCS3 with " +
           "the PS3 firmware and the game's 1.04 update installed. Setup decrypts and translates YOUR copy " +
           "on this PC; nothing of the game is downloaded or shared.") 16 42 728 48 $false | Out-Null
$disc = New-PathRow "Game disc folder (the folder that holds PS3_GAME):" 94 $(if (Test-Path (Join-Path $root "game\disc\PS3_GAME")) { (Get-Item (Join-Path $root "game\disc")).FullName } else { "" }) "The game's disc folder: the one that holds PS3_GAME"
$rpcs3 = New-PathRow "RPCS3 folder (the folder that holds rpcs3.exe):" 144 $(if ($saved["rpcs3"]) { $saved["rpcs3"] } else { "" }) "The RPCS3 folder: the one that holds rpcs3.exe"

$list = New-Object System.Windows.Forms.ListView
$list.Location = New-Object System.Drawing.Point(16, 198); $list.Size = New-Object System.Drawing.Size(728, 250)
$list.View = "Details"; $list.FullRowSelect = $true; $list.HeaderStyle = "Nonclickable"
[void]$list.Columns.Add("", 60); [void]$list.Columns.Add("Check", 230); [void]$list.Columns.Add("Details", 420)
$f.Controls.Add($list)

$btnCheck = New-Button "Check again" 16 456 120
$btnTools = New-Button "Install missing tools" 144 456 170
$btnBuild = New-Button "Build the game" 322 456 150
$btnPlay = New-Button "Play" 480 456 120
$btnLog = New-Button "Open log" 608 456 136
$status = New-Label "" 16 492 728 20 $false
$log = New-Object System.Windows.Forms.TextBox
$log.Location = New-Object System.Drawing.Point(16, 514); $log.Size = New-Object System.Drawing.Size(728, 112)
$log.Multiline = $true; $log.ReadOnly = $true; $log.ScrollBars = "Vertical"; $log.Font = New-Object System.Drawing.Font("Consolas", 8.5)
$f.Controls.Add($log)

$script:missingTools = @()
function Update-Checks {
    $list.Items.Clear()
    $rows = Get-SetupChecks -Disc $disc.Text.Trim() -Rpcs3 $rpcs3.Text.Trim()
    $script:missingTools = @()
    $blocked = $false
    foreach ($r in $rows) {
        $mark = if ($r.Ok -eq $true) { "OK" } elseif ($r.Ok -eq $false) { "MISSING" } else { "note" }
        $it = New-Object System.Windows.Forms.ListViewItem($mark)
        [void]$it.SubItems.Add($r.Name); [void]$it.SubItems.Add([string]$r.Detail)
        $it.ForeColor = if ($r.Ok -eq $true) { [System.Drawing.Color]::DarkGreen } elseif ($r.Ok -eq $false) { [System.Drawing.Color]::Firebrick } else { [System.Drawing.Color]::DarkGoldenrod }
        [void]$list.Items.Add($it)
        if ($r.Ok -eq $false) {
            $blocked = $true
            if ($r.Detail -match 'winget install ([\w\.\-]+)') { $script:missingTools += $Matches[1] }
        }
    }
    $busy = $script:proc -and -not $script:proc.HasExited
    $btnTools.Enabled = ($script:missingTools.Count -gt 0) -and -not $busy
    $btnBuild.Enabled = (-not $blocked) -and -not $busy
    $btnPlay.Enabled = (Test-Path $exe) -and -not $busy
    $status.Text = if ($busy) { "Building..." } elseif ($blocked) { "Fix the MISSING items, then Build." } elseif (Test-Path $exe) { "Built. Play, or Build again after an update." } else { "Ready to build (15-40 minutes, one time)." }
}

$btnCheck.Add_Click({ Update-Checks })
$btnTools.Add_Click({
    $ids = $script:missingTools | Select-Object -Unique
    $cmds = foreach ($id in $ids) {
        if ($id -eq "Microsoft.VisualStudio.2022.BuildTools") {
            "winget install --id $id -e --accept-source-agreements --accept-package-agreements --override `"--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`""
        } else { "winget install --id $id -e --accept-source-agreements --accept-package-agreements" }
    }
    $msg = "Setup will open a window that installs, with Microsoft's winget:`n`n" + ($ids -join "`n") + "`n`nWindows may ask for permission. When it finishes, close Setup and start it again (new tools need a fresh start)."
    if ([System.Windows.Forms.MessageBox]::Show($msg, "Install missing tools", "OKCancel", "Information") -eq "OK") {
        $script = ($cmds -join "; ") + "; Write-Host ''; Write-Host 'Done. Close this window, then close and restart Setup.'; Read-Host"
        Start-Process powershell -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $script
    }
})
$btnBuild.Add_Click({
    if (Get-Process rpcs3 -ErrorAction SilentlyContinue) { [System.Windows.Forms.MessageBox]::Show("Close RPCS3 first: Setup uses it to decrypt your game.", "Setup") | Out-Null; return }
    # The build downloads its tools (kept for quick updates); Microsoft's C++ files need their license.
    $lic = "Setup downloads the build tools into this folder (about 1.3 GB, kept for quick updates; nothing is installed).`n`n" +
           "Microsoft's C++ files (MSVC and the Windows SDK) come under the Visual Studio Build Tools license:`n" +
           "https://go.microsoft.com/fwlink/?LinkId=2179911`n`nDo you accept Microsoft's license terms?"
    if ([System.Windows.Forms.MessageBox]::Show($lic, "Microsoft's license", "YesNo", "Question") -ne "Yes") { return }
    New-Item -ItemType Directory -Force (Split-Path $logPath) | Out-Null
    Set-Content -Path $logPath -Value "" -Encoding UTF8
    $a = "-NoProfile -ExecutionPolicy Bypass -Command `"& '$(Join-Path $PSScriptRoot 'setup_build.ps1')' -Disc '$($disc.Text.Trim())' -Rpcs3 '$($rpcs3.Text.Trim())' -AcceptMsvcLicense -Cleanup *>&1 | Out-File -Encoding utf8 -FilePath '$logPath'; exit `$LASTEXITCODE`""
    $script:proc = Start-Process powershell -ArgumentList $a -WindowStyle Hidden -PassThru
    $btnBuild.Enabled = $false; $btnPlay.Enabled = $false; $btnTools.Enabled = $false
    $status.Text = "Building... (you can leave this window open; the log is below)"
    $timer.Start()
})
$btnPlay.Add_Click({
    Start-Process powershell -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$(Join-Path $PSScriptRoot 'play.ps1')`"" -WorkingDirectory $root -WindowStyle Minimized
})
$btnLog.Add_Click({ if (Test-Path $logPath) { Start-Process notepad $logPath } })

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000
$timer.Add_Tick({
    if (Test-Path $logPath) {
        try {
            $fs = [System.IO.File]::Open($logPath, "Open", "Read", "ReadWrite")
            $sr = New-Object System.IO.StreamReader($fs); $text = $sr.ReadToEnd(); $sr.Close()
            $lines = $text -split "`r?`n" | Where-Object { $_ -ne "" }
            $log.Text = (($lines | Select-Object -Last 40) -join "`r`n")
            $log.SelectionStart = $log.Text.Length; $log.ScrollToCaret()
            $step = $lines | Where-Object { $_ -like "== *" } | Select-Object -Last 1
            if ($step) { $status.Text = $step.Substring(3) }
        } catch {}
    }
    if ($script:proc -and $script:proc.HasExited) {
        $timer.Stop()
        $ok = Test-Path $exe
        Update-Checks
        $status.Text = if ($script:proc.ExitCode -eq 0 -and $ok) { "Done: the game is built. Press Play (or use the desktop shortcut)." } else { "The build stopped: see the log (Open log)." }
    }
})

$f.Add_Shown({ Update-Checks })
[void]$f.ShowDialog()
