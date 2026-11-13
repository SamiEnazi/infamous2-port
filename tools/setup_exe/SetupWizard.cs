// SetupWizard.cs -- Setup.exe of the inFAMOUS 2 Windows port: one big Install button.
//
// Install does everything, asking only what it truly cannot find out:
//   1. Setup.exe downloaded on its own: the port as one ZIP file from GitHub (no git) into a folder
//      next to Setup.exe (else %LOCALAPPDATA%\inFAMOUS2-port), then Setup.exe copies itself there.
//   2. RPCS3 and the game: found on this PC (RPCS3's own game list for the game); a small "Where
//      is ...?" dialog only when one is not found.
//   3. The checks of tools\setup_common.ps1; a one-line reason only for what the player must fix
//      (the PS3 firmware or the 1.04 update missing in RPCS3, a PC that can't run the port).
//   4. tools\setup_build.ps1 -AcceptMsvcLicense -Cleanup [-Pgo]: it downloads its own build tools into
//      tools-cache\ (portable: nothing is installed, no admin), builds, and deletes the tools again.
//      Its lines "== [n/10] <step>" (12 steps with -Pgo), "  progress: <n>% <what>", "== FAILED: <why>"
//      drive the bar; "  compile log: <file>" names the log whose ninja lines to follow, "  player-note:
//      <text>" is shown under the bar for that step, "  pgo: used (...)" / "  pgo: not used: <why>" end
//      the Done line. -Pgo = the "Optimise for this PC" box (on by default, remembered per folder in
//      work\setup-no-pgo): a training build plays the game by itself for a few minutes (tools\pgo_train.ps1),
//      then the game is compiled with what it learned; it never makes Setup fail.
//   5. A big Play button.
// Pressing Install accepts Microsoft's license for the C++ files (the line under the button says so).
//
// build.cmd compiles it with the C# compiler that ships with Windows (.NET Framework 4.5+, C# 5):
// no $"", no => members, no ?., no nameof, no out var. Non-ASCII text is written as \u escapes.
// The port's scripts run as "powershell -ExecutionPolicy RemoteSigned -File <script>" (Util.PowerShellFile).
//
// Test switches (README.md): --test-build, --rpcs3 <folder>, --disc <folder> (or --game), --folder <folder>,
// --no-shortcut, --no-cleanup, --only-cache, --no-pgo, --exit-when-done, --page done|ask; --pgo with --prepare
// or --update --no-window (they do not train unless asked: the training opens the game's window).
// The updater (tools\update_check.ps1, F2 > Update in the game): --update --folder <port folder> [--to v0.5.1]
// = "Update to the latest version" at once, in the window; with --no-window [--dry-run] without one (class Headless).

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

// The file's version information (Explorer > Properties > Details). The manifest is setup.manifest (build.cmd).
[assembly: AssemblyTitle("inFAMOUS 2 Native PC Port Setup")]
[assembly: AssemblyDescription("Open-source setup of the inFAMOUS 2 Native PC Port: builds the game for Windows from the player's own copy (github.com/SamiEnazi/infamous2-port)")]
[assembly: AssemblyCompany("Sami Enazi")]
[assembly: AssemblyProduct("inFAMOUS 2 Native PC Port (unofficial fan project)")]
[assembly: AssemblyCopyright("Copyright (c) 2026 Sami Enazi. MIT license.")]
[assembly: AssemblyTrademark("inFAMOUS is a trademark of Sony Interactive Entertainment LLC. Not affiliated with Sony or Sucker Punch.")]
[assembly: AssemblyVersion("0.5.2.0")]
[assembly: AssemblyFileVersion("0.5.2.0")]
[assembly: AssemblyInformationalVersion("0.5.2")]

namespace Inf2Setup
{
    // ------------------------------------------------------------------ start

    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            try { Native.SetProcessDPIAware(); } catch (Exception) { }
            // GitHub needs TLS 1.2; an app built without a target framework gets the old default
            try { ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072; } catch (Exception) { }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
            Application.ThreadException += delegate(object s, ThreadExceptionEventArgs e) { Oops(e.Exception); };
            AppDomain.CurrentDomain.UnhandledException += delegate(object s, UnhandledExceptionEventArgs e) { Oops(e.ExceptionObject as Exception); };

            Opts opt = Opts.Parse(args);
            // RecompLauncher (recomplauncher.json): --prepare and --play, without a window
            if (opt.Prepare || opt.Play) return Headless.Run(opt, Path.GetDirectoryName(Path.GetFullPath(Application.ExecutablePath)));
            // the updater without a window (tests, scripts)
            if (opt.Update && opt.NoWindow) return Headless.Run(opt, Path.GetDirectoryName(Path.GetFullPath(Application.ExecutablePath)));
            Theme.Init(FitScale());
            string exeDir = Path.GetDirectoryName(Path.GetFullPath(Application.ExecutablePath));
            // --update works on --folder (play.ps1 starts a Setup.exe it built in work\update\setup): that folder's lock
            string lockDir = opt.Update && !string.IsNullOrEmpty(opt.Folder) ? Path.GetFullPath(opt.Folder).TrimEnd('\\') : exeDir;
            bool first;
            // one Setup per folder: two builds in the same folder would trip over each other
            using (Mutex m = new Mutex(true, "inFAMOUS2-port-setup-" + Util.Hash(lockDir.ToLowerInvariant()), out first))
            {
                if (!first)
                {
                    MessageBox.Show("Setup is already open.", MainForm.Title, MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return 3;
                }
                MainForm f = new MainForm(opt, exeDir);
                Application.Run(f);
                return f.ExitCode;
            }
        }

        // The screen's DPI scale, lowered when the window would not fit.
        static float FitScale()
        {
            float s = 1f;
            try { using (Graphics g = Graphics.FromHwnd(IntPtr.Zero)) s = g.DpiX / 96f; } catch (Exception) { }
            try
            {
                Rectangle wa = Screen.PrimaryScreen.WorkingArea;
                float fitH = (wa.Height - 16) / (float)(MainForm.BaseH + MainForm.LogH + 40);
                float fitW = (wa.Width - 16) / (float)(MainForm.BaseW + 16);
                s = Math.Min(s, Math.Min(fitH, fitW));
            }
            catch (Exception) { }
            return Math.Max(0.75f, s);
        }

        static void Oops(Exception e)
        {
            try
            {
                MessageBox.Show("Setup ran into a problem it did not expect:\n\n" + (e != null ? e.Message : "unknown error") +
                    "\n\nPlease report it on GitHub (SamiEnazi/infamous2-port, Issues) with a screenshot of this message.",
                    MainForm.Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            catch (Exception) { }
        }
    }

    // Command-line switches, for testing (README.md), and RecompLauncher's --prepare / --play (class Headless).
    sealed class Opts
    {
        public bool Prepare, Play, DryRun;
        public List<string> PlayArgs = new List<string>();   // after --play: play.ps1's own options
        public string Page, Disc, Rpcs3, Folder;
        public bool TestBuild, NoShortcut, ExitWhenDone, NoCleanup, OnlyCache, TestFailBuild;
        public bool NoPgo, Pgo;          // the window's "Optimise for this PC" box off; --prepare / --no-window with it
        public bool Update, NoWindow;    // --update [--no-window]: the newest port over --folder (the updater)
        public string To;                // --to v0.5.1: the version the update brings (only shown)

        public static Opts Parse(string[] a)
        {
            Opts o = new Opts();
            for (int i = 0; i < a.Length; i++)
            {
                string k = a[i].ToLowerInvariant();
                string v = i + 1 < a.Length ? a[i + 1] : null;
                switch (k)
                {
                    case "--prepare": o.Prepare = true; break;
                    case "--dry-run": o.DryRun = true; break;
                    case "--play":
                        o.Play = true;
                        for (i++; i < a.Length; i++)
                        {
                            if (string.Equals(a[i], "--dry-run", StringComparison.OrdinalIgnoreCase)) o.DryRun = true;
                            else o.PlayArgs.Add(a[i]);
                        }
                        break;
                    case "--page": o.Page = v; i++; break;
                    case "--disc":
                    case "--game": o.Disc = v; i++; break;   // --game: the name launchers and the Recomp Standards use
                    case "--rpcs3": o.Rpcs3 = v; i++; break;
                    case "--folder": o.Folder = v; i++; break;
                    case "--test-build": o.TestBuild = true; break;
                    case "--no-shortcut": o.NoShortcut = true; break;
                    case "--exit-when-done": o.ExitWhenDone = true; break;
                    case "--no-cleanup": o.NoCleanup = true; break;
                    case "--only-cache": o.OnlyCache = true; break;
                    case "--test-fail-build": o.TestFailBuild = true; break;
                    case "--no-pgo": o.NoPgo = true; break;
                    case "--pgo": o.Pgo = true; break;
                    case "--update": o.Update = true; break;
                    case "--no-window": o.NoWindow = true; break;
                    case "--to": o.To = v; i++; break;
                }
            }
            return o;
        }
    }

    // ------------------------------------------------------------------ look

    // The game's menu theme (toolkit vk_overlay_theme.cpp, "hero"): charcoal panels, electric blue.
    static class Theme
    {
        public static readonly Color Bg = Color.FromArgb(14, 16, 21);
        public static readonly Color Card = Color.FromArgb(22, 26, 34);
        public static readonly Color CardHi = Color.FromArgb(30, 36, 47);
        public static readonly Color Line = Color.FromArgb(44, 52, 67);
        public static readonly Color Text = Color.FromArgb(236, 241, 247);
        public static readonly Color Dim = Color.FromArgb(153, 166, 184);
        public static readonly Color Accent = Color.FromArgb(63, 169, 255);
        public static readonly Color AccentHover = Color.FromArgb(104, 188, 255);
        public static readonly Color AccentDown = Color.FromArgb(40, 140, 225);
        public static readonly Color AccentHi = Color.FromArgb(150, 210, 255);
        public static readonly Color SelBg = Color.FromArgb(17, 40, 64);
        public static readonly Color Good = Color.FromArgb(74, 201, 120);
        public static readonly Color Bad = Color.FromArgb(255, 110, 110);
        public static readonly Color Ink = Color.FromArgb(6, 18, 32);      // text on the blue
        public static readonly Color LogBg = Color.FromArgb(9, 11, 15);

        public static float Scale = 1f;
        public static Font Body, Small, Bold, H1, H2, Btn, BtnHuge, Mono;

        // Fonts in pixels times Scale, so text and layout always scale together.
        public static void Init(float scale)
        {
            Scale = scale;
            Body = Px("Segoe UI", 14.5f, FontStyle.Regular);
            Small = Px("Segoe UI", 12.5f, FontStyle.Regular);
            Bold = Px("Segoe UI", 15f, FontStyle.Bold);
            H1 = Px("Segoe UI", 34f, FontStyle.Bold);
            H2 = Px("Segoe UI", 17f, FontStyle.Bold);
            Btn = Px("Segoe UI", 13.5f, FontStyle.Bold);
            BtnHuge = Px("Segoe UI", 24f, FontStyle.Bold);
            Mono = Px("Consolas", 12f, FontStyle.Regular);
        }

        static Font Px(string face, float px, FontStyle st) { return new Font(face, px * Scale, st, GraphicsUnit.Pixel); }

        public static int S(double v) { return (int)Math.Round(v * Scale); }

        public static GraphicsPath Round(RectangleF r, float radius)
        {
            GraphicsPath p = new GraphicsPath();
            float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
            if (d <= 1) { p.AddRectangle(r); return p; }
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public static LinkLabel Link(string text, Font f)
        {
            LinkLabel l = new LinkLabel();
            l.Text = text; l.Font = f; l.AutoSize = true; l.UseMnemonic = false;
            l.LinkColor = Accent; l.ActiveLinkColor = AccentHi; l.VisitedLinkColor = Accent;
            l.LinkBehavior = LinkBehavior.HoverUnderline; l.ForeColor = Dim;
            l.BackColor = Bg;
            return l;
        }
    }

    interface IAnimated { bool Animating { get; } }

    // One shared animation clock for the progress bar's shine.
    static class Anim
    {
        public static int Frame;
        static readonly List<Control> live = new List<Control>();

        public static void Add(Control c)
        {
            live.Add(c);
            c.Disposed += delegate { live.Remove(c); };
        }

        public static void Tick()
        {
            Frame++;
            foreach (Control c in live.ToArray())
            {
                IAnimated a = c as IAnimated;
                if (a != null && a.Animating && c.Visible && c.IsHandleCreated) c.Invalidate();
            }
        }
    }

    static class Native
    {
        [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
        [DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
        [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint flags);

        // Keep the PC from going to sleep during a long build.
        public static void KeepAwake(bool on)
        {
            try { SetThreadExecutionState(on ? 0x80000001u : 0x80000000u); } catch (Exception) { }
        }

        // A dark title bar to match (Windows 10 1809+ and 11; ignored elsewhere).
        public static void DarkTitle(IntPtr h, Color caption)
        {
            try
            {
                int on = 1;
                if (DwmSetWindowAttribute(h, 20, ref on, 4) != 0) DwmSetWindowAttribute(h, 19, ref on, 4);
                int c = caption.R | (caption.G << 8) | (caption.B << 16);
                DwmSetWindowAttribute(h, 35, ref c, 4);
            }
            catch (Exception) { }
        }
    }

    // The icon: embedded by build.cmd (/resource), else tools\setup.ico beside Setup.exe.
    static class Res
    {
        static byte[] ico;

        static byte[] IcoBytes(string dir)
        {
            if (ico != null) return ico;
            try
            {
                using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("setup.ico"))
                {
                    if (s != null) { MemoryStream m = new MemoryStream(); s.CopyTo(m); ico = m.ToArray(); return ico; }
                }
            }
            catch (Exception) { }
            try
            {
                string p = Path.Combine(dir, "tools\\setup.ico");
                if (File.Exists(p)) ico = File.ReadAllBytes(p);
            }
            catch (Exception) { }
            return ico;
        }

        public static Icon WindowIcon(string dir)
        {
            byte[] b = IcoBytes(dir);
            if (b != null) { try { return new Icon(new MemoryStream(b)); } catch (Exception) { } }
            try { return Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { return null; }
        }

        // The biggest picture in the .ico (the 256 px one is a PNG), for the logo.
        public static Image Logo(string dir)
        {
            byte[] b = IcoBytes(dir);
            if (b == null || b.Length < 22) return null;
            try
            {
                int n = BitConverter.ToUInt16(b, 4), best = -1, bestW = 0;
                for (int i = 0; i < n; i++)
                {
                    int o = 6 + 16 * i;
                    int w = b[o] == 0 ? 256 : b[o];
                    if (w > bestW) { bestW = w; best = o; }
                }
                if (best < 0) return null;
                int size = (int)BitConverter.ToUInt32(b, best + 8), off = (int)BitConverter.ToUInt32(b, best + 12);
                if (b[off] == 0x89 && b[off + 1] == 0x50)
                {
                    using (MemoryStream m = new MemoryStream(b, off, size))
                    using (Image img = Image.FromStream(m))
                        return new Bitmap(img);
                }
                using (Icon ic = new Icon(new MemoryStream(b), bestW, bestW)) return ic.ToBitmap();
            }
            catch (Exception) { return null; }
        }
    }

    // ------------------------------------------------------------------ helpers

    static class Util
    {
        public static readonly string[] Titles = { "BCES01143", "BCES01144", "BCES01229", "BCUS98125", "NPUA80638", "NPEA00318" };

        // Re-read PATH from the registry (Machine + User), so the checks see what the PC has now.
        public static void RefreshPath()
        {
            try
            {
                string m = Environment.GetEnvironmentVariable("Path", EnvironmentVariableTarget.Machine) ?? "";
                string u = Environment.GetEnvironmentVariable("Path", EnvironmentVariableTarget.User) ?? "";
                string cur = Environment.GetEnvironmentVariable("Path") ?? "";
                HashSet<string> seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                List<string> parts = new List<string>();
                foreach (string s in (m + ";" + u + ";" + cur).Split(';'))
                {
                    string t = Environment.ExpandEnvironmentVariables(s.Trim());
                    if (t.Length == 0) continue;
                    if (seen.Add(t.TrimEnd('\\'))) parts.Add(t);
                }
                Environment.SetEnvironmentVariable("Path", string.Join(";", parts));
            }
            catch (Exception) { }
        }

        public static string PowerShellExe()
        {
            string p = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell\\v1.0\\powershell.exe");
            return File.Exists(p) ? p : "powershell.exe";
        }

        // PowerShell running one of the port's own scripts: -File (no inline code), under this process's
        // RemoteSigned policy, which also runs them on a PC left at Windows' default (Restricted). The
        // scripts Setup downloads and unpacks itself carry no "downloaded from the internet" mark.
        public static string PowerShellFile(string ps1, bool interactive)
        {
            return "-NoProfile " + (interactive ? "" : "-NonInteractive ") + "-ExecutionPolicy RemoteSigned -File " + Arg(ps1);
        }

        // RemoteSigned refuses a script that carries the mark: a release ZIP downloaded with a browser and
        // unpacked with Explorer. PowerShell's own error says so in words the player can't act on.
        public static bool ScriptBlocked(string stderr)
        {
            return stderr != null && (stderr.Contains("PSSecurityException") || stderr.Contains("UnauthorizedAccess"));
        }

        // The fix the player would do by hand (Properties > Unblock), done for the port's own folder only: the
        // "downloaded from the internet" mark is the file's Zone.Identifier stream. Folders that hold no files
        // from the ZIP (game\ with its links to the player's disc, the build tools, builds, git) are not entered,
        // nor any linked folder. Returns how many files lost the mark.
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool DeleteFileW(string path);
        public static int UnblockTree(string root)
        {
            int n = 0;
            Stack<string> dirs = new Stack<string>();
            dirs.Push(root);
            while (dirs.Count > 0)
            {
                string d = dirs.Pop();
                try
                {
                    foreach (string f in Directory.GetFiles(d))
                        if (DeleteFileW(f + ":Zone.Identifier")) n++;
                    foreach (string sub in Directory.GetDirectories(d))
                    {
                        string name = Path.GetFileName(sub).ToLowerInvariant();
                        if (d == root && (name == "game" || name == "tools-cache" || name == "work" || name == "deps" || name == ".git")) continue;
                        if (name.StartsWith("build")) continue;
                        if ((File.GetAttributes(sub) & FileAttributes.ReparsePoint) != 0) continue;
                        dirs.Push(sub);
                    }
                }
                catch (Exception) { }
            }
            return n;
        }

        public const string BlockedHelp = "Windows marked the port's files as downloaded from the internet, so PowerShell won't run its " +
            "scripts. Right-click the ZIP you downloaded, choose Properties, tick Unblock, press OK and unpack it again " +
            "(or use Setup.exe on its own: it downloads the port itself)";

        // One command-line argument, quoted the way Windows splits them again.
        public static string Arg(string s)
        {
            if (s.Length > 0 && s.IndexOfAny(new char[] { ' ', '\t', '"' }) < 0) return s;
            StringBuilder b = new StringBuilder("\"");
            int bs = 0;
            foreach (char c in s)
            {
                if (c == '\\') { bs++; continue; }
                if (c == '"') { b.Append('\\', bs * 2 + 1); b.Append('"'); bs = 0; continue; }
                b.Append('\\', bs); bs = 0;
                b.Append(c);
            }
            b.Append('\\', bs * 2);
            b.Append('"');
            return b.ToString();
        }

        // Only Windows' own folders: the build's PATH in the --only-cache test.
        public static string WindowsPath()
        {
            string w = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            return string.Join(";", new string[] { Path.Combine(w, "System32"), w, Path.Combine(w, "System32\\Wbem"),
                                                   Path.Combine(w, "System32\\WindowsPowerShell\\v1.0") });
        }

        public static bool HasPort(string dir)
        {
            try { return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "tools\\setup_common.ps1")); }
            catch (Exception) { return false; }
        }

        // The port with its engine and port base, folders of the one repository since v0.5.0 (a finished
        // download or a git clone).
        public static bool PortComplete(string dir)
        {
            try
            {
                return HasPort(dir) && File.Exists(Path.Combine(dir, "engine\\CMakeLists.txt")) &&
                       File.Exists(Path.Combine(dir, "base\\infamous2-lle\\CMakeLists.txt"));
            }
            catch (Exception) { return false; }
        }

        public static string BuiltExe(string repo) { return Path.Combine(repo, "work\\port\\build\\Infamous2Recomp.exe"); }

        // An install Setup made before, where DefaultTarget would put it (no folder is created here).
        public static string ExistingInstall(string exeDir)
        {
            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string sys = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows));
            if (string.IsNullOrEmpty(sys)) sys = "C:\\";
            foreach (string c in new string[] { Path.Combine(exeDir, "inFAMOUS2-port"), Path.Combine(local, "inFAMOUS2-port"), Path.Combine(sys, "inFAMOUS2-port") })
                if (HasPort(c) && CanLink(c)) return c;   // one on a drive that cannot link never finished
            return null;
        }

        // port-version.txt (written by the download): the port's commit, or null. Since v0.5.0 the engine and
        // the port base are in that commit (engine\, base\); the "toolkit" and "community/..." pin lines of
        // older downloads are only read past.
        public static string ReadPortVersion(string root)
        {
            try
            {
                string f = Path.Combine(root, "port-version.txt");
                if (!File.Exists(f)) return null;
                string v = null;
                foreach (string ln in File.ReadAllLines(f))
                {
                    string[] t = ln.Trim().Split(' ');
                    if (t.Length == 2 && t[0] == "SamiEnazi/infamous2-port") v = t[1];
                }
                return v;
            }
            catch (Exception) { return null; }
        }

        // port-version.txt names the toolkit / port base pins (written by a Setup.exe of v0.4.2 or older, or a
        // release zip before v0.5.0): toolkit\ and community\ then hold only that downloaded source.
        public static bool HadPins(string root)
        {
            try
            {
                string f = Path.Combine(root, "port-version.txt");
                if (!File.Exists(f)) return false;
                foreach (string ln in File.ReadAllLines(f))
                {
                    string[] t = ln.Trim().Split(' ');
                    if (t.Length == 3 && t[0] == "toolkit") return true;
                }
                return false;
            }
            catch (Exception) { return false; }
        }

        // What an update never writes into: the player's folders and files, the build's own folders, the
        // engine and the port base (those are swapped whole), and the toolkit\ and community\ folders of an
        // install from before v0.5.0. game\disc is a junction to the player's disc.
        static readonly string[] KeepTop = { "game", "work", "play", "saves-backup", "tools-cache", "deps", "build", "eboot", "engine", "base", "toolkit", "community" };
        // Pure source inside the port's download, replaced whole on an update (a file deleted upstream must not
        // stay behind: the engine's CMake builds every file it finds in libs\ and runtime\).
        public static readonly string[] SwapTop = { "engine", "base" };
        static readonly string[] KeepFiles = { "input.toml", "config.local.json", "Setup.exe", "port-version.txt", ".zip-source" };

        public static bool Protected(string rel)
        {
            string first = rel.Split('\\')[0];
            foreach (string t in KeepTop) if (string.Equals(first, t, StringComparison.OrdinalIgnoreCase)) return true;
            foreach (string f in KeepFiles) if (string.Equals(rel, f, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        static bool IsLink(FileSystemInfo i) { return (i.Attributes & FileAttributes.ReparsePoint) != 0; }

        // True when no folder between root (not included) and dir is a link (a junction or a symlink).
        public static bool NoLinkBetween(string root, string dir, Dictionary<string, bool> seen)
        {
            string r = Path.GetFullPath(root).TrimEnd('\\'), d = Path.GetFullPath(dir).TrimEnd('\\');
            List<string> walked = new List<string>();
            bool ok = true;
            while (d.Length > r.Length && d.StartsWith(r + "\\", StringComparison.OrdinalIgnoreCase))
            {
                bool known;
                if (seen.TryGetValue(d, out known)) { ok = known; break; }
                walked.Add(d);
                DirectoryInfo di = new DirectoryInfo(d);
                if (di.Exists && IsLink(di)) { ok = false; break; }
                d = Path.GetDirectoryName(d);
            }
            foreach (string w in walked) seen[w] = ok;
            return ok;
        }

        // The first link at or inside dir (never followed), or null.
        public static string FindLink(string dir)
        {
            DirectoryInfo top = new DirectoryInfo(dir);
            if (!top.Exists) return null;
            if (IsLink(top)) return top.FullName;
            Stack<DirectoryInfo> st = new Stack<DirectoryInfo>();
            st.Push(top);
            while (st.Count > 0)
            {
                foreach (FileSystemInfo e in st.Pop().GetFileSystemInfos())
                {
                    if (IsLink(e)) return e.FullName;
                    DirectoryInfo sub = e as DirectoryInfo;
                    if (sub != null) st.Push(sub);
                }
            }
            return null;
        }

        // Delete a folder Setup made or replaces, and refuse if a link is anywhere inside it.
        public static void DeleteTree(string dir)
        {
            if (!Directory.Exists(dir)) return;
            string link = FindLink(dir);
            if (link != null) throw new IOException("Setup will not delete " + dir + ": it contains a link (" + link + ")");
            foreach (string f in Directory.GetFiles(dir, "*", SearchOption.AllDirectories))
            {
                FileAttributes a = File.GetAttributes(f);
                if ((a & FileAttributes.ReadOnly) != 0) File.SetAttributes(f, a & ~FileAttributes.ReadOnly);
            }
            Directory.Delete(dir, true);
        }

        // Copy a folder's files (links are skipped, never followed); returns how many.
        public static int CopyTree(string src, string dst)
        {
            int n = 0;
            DirectoryInfo top = new DirectoryInfo(src);
            if (!top.Exists || IsLink(top)) return 0;
            Directory.CreateDirectory(dst);
            foreach (FileSystemInfo e in top.GetFileSystemInfos())
            {
                if (IsLink(e)) continue;
                if (e is DirectoryInfo) n += CopyTree(e.FullName, Path.Combine(dst, e.Name));
                else { File.Copy(e.FullName, Path.Combine(dst, e.Name), true); n++; }
            }
            return n;
        }

        // Two files with the same bytes (an update keeps such a file's time: the build files stay valid for it).
        public static bool SameFile(string a, string b)
        {
            try
            {
                FileInfo fa = new FileInfo(a), fb = new FileInfo(b);
                if (!fa.Exists || !fb.Exists || fa.Length != fb.Length) return false;
                using (FileStream x = new FileStream(a, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1 << 16))
                using (FileStream y = new FileStream(b, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1 << 16))
                {
                    byte[] p = new byte[1 << 16], q = new byte[1 << 16];
                    while (true)
                    {
                        int n = x.Read(p, 0, p.Length);
                        if (n <= 0) return true;
                        int m = 0;
                        while (m < n) { int r = y.Read(q, m, n - m); if (r <= 0) return false; m += r; }
                        for (int i = 0; i < n; i++) if (p[i] != q[i]) return false;
                    }
                }
            }
            catch (Exception) { return false; }
        }

        // The drive of `path` can hold the junction Setup makes (game\disc -> the player's game folder):
        // a local NTFS or ReFS drive. FAT32/exFAT (USB sticks, many external disks) and network drives
        // cannot, and Windows then fails with "Incorrect function" at the start of the build (GitHub
        // issue #2: Setup.exe on such a drive stopped at 23%).
        public static bool CanLink(string path)
        {
            return DriveProblem(path) == null;
        }
        // null when the drive is fine, else what it is ("exFAT", "FAT32", "a network drive", ...).
        public static string DriveProblem(string path)
        {
            try
            {
                string root = Path.GetPathRoot(Path.GetFullPath(path));
                if (string.IsNullOrEmpty(root) || root.StartsWith("\\\\", StringComparison.Ordinal)) return "a network drive";
                DriveInfo di = new DriveInfo(root);
                if (di.DriveType == DriveType.Network) return "a network drive";
                if (!di.IsReady) return "a drive that is not ready";
                string f = di.DriveFormat;
                if (string.Equals(f, "NTFS", StringComparison.OrdinalIgnoreCase) || string.Equals(f, "ReFS", StringComparison.OrdinalIgnoreCase)) return null;
                return "a " + f + " drive";
            }
            catch (Exception) { return "a drive Setup could not read"; }
        }
        // The log line when a downloaded-alone Setup.exe could not use the folder next to itself
        // because of its drive, or null.
        public static string LinkNote(string exeDir, string target)
        {
            string p = DriveProblem(exeDir);
            if (p == null || string.Equals(Path.GetFullPath(target), Path.GetFullPath(Path.Combine(exeDir, "inFAMOUS2-port")), StringComparison.OrdinalIgnoreCase)) return null;
            return "Setup.exe is on " + p + ", which cannot hold the link to your game folder (that needs a local NTFS drive): installing into " + target + " instead.";
        }

        // Where a downloaded-alone Setup.exe puts the game: next to itself, else in the user's local
        // app data, else on the system drive; a plain path (build tools dislike other alphabets) with
        // about 15 GB free, on a drive that can hold the game\disc link (CanLink). An earlier download
        // is used again (not one on a drive that cannot link: that install could never finish).
        public static string DefaultTarget(string exeDir)
        {
            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string sys = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows));
            if (string.IsNullOrEmpty(sys)) sys = "C:\\";
            string[] cands = { Path.Combine(exeDir, "inFAMOUS2-port"), Path.Combine(local, "inFAMOUS2-port"), Path.Combine(sys, "inFAMOUS2-port") };
            foreach (string c in cands) if (HasPort(c) && CanLink(c)) return c;
            foreach (string c in cands)
            {
                try
                {
                    if (!IsAscii(c)) continue;
                    if (!CanLink(c)) continue;
                    if (Directory.Exists(c) && !EmptyDir(c)) continue;
                    DriveInfo di = new DriveInfo(Path.GetPathRoot(c));
                    if (!di.IsReady || di.AvailableFreeSpace < (15L << 30)) continue;
                    string parent = Path.GetDirectoryName(c);
                    Directory.CreateDirectory(parent);
                    string probe = Path.Combine(parent, ".inf2-setup-probe");
                    File.WriteAllText(probe, "");
                    File.Delete(probe);
                    return c;
                }
                catch (Exception) { }
            }
            return cands[1];
        }

        // config.local.json is written by setup_build.ps1 (it keeps "rpcs3").
        public static string ReadConfig(string repo, string key)
        {
            try
            {
                string f = Path.Combine(repo, "config.local.json");
                if (!File.Exists(f)) return null;
                Dictionary<string, object> d = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(f)) as Dictionary<string, object>;
                object v;
                if (d != null && d.TryGetValue(key, out v) && v is string) return (string)v;
            }
            catch (Exception) { }
            return null;
        }

        static bool IsRpcs3(string d)
        {
            try { return !string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, "rpcs3.exe")); } catch (Exception) { return false; }
        }

        // rpcs3.exe in dir or one folder below it (or two, for the top of a drive)
        static string Look(string dir, int depth, Stopwatch sw)
        {
            if (sw.ElapsedMilliseconds > 6000 || string.IsNullOrEmpty(dir)) return null;
            try
            {
                if (!Directory.Exists(dir)) return null;
                if (IsRpcs3(dir)) return dir;
                if (depth == 0) return null;
                foreach (string sub in Directory.GetDirectories(dir))
                {
                    string n = Path.GetFileName(sub);
                    if (n.StartsWith("$") || n.Equals("Windows", StringComparison.OrdinalIgnoreCase) ||
                        n.Equals("System Volume Information", StringComparison.OrdinalIgnoreCase) ||
                        n.Equals("ProgramData", StringComparison.OrdinalIgnoreCase) || n.Equals("Users", StringComparison.OrdinalIgnoreCase))
                        continue;
                    string r = Look(sub, depth - 1, sw);
                    if (r != null) return r;
                }
            }
            catch (Exception) { }
            return null;
        }

        // RPCS3 is portable: the saved choice, a running RPCS3, next to Setup.exe, the user's folders,
        // Program Files, the top of each drive. A few seconds at most.
        public static string FindRpcs3(string exeDir, string repo)
        {
            if (repo != null) { string c = ReadConfig(repo, "rpcs3"); if (IsRpcs3(c)) return c; }
            try
            {
                foreach (Process p in Process.GetProcessesByName("rpcs3"))
                {
                    try { string d = Path.GetDirectoryName(p.MainModule.FileName); if (IsRpcs3(d)) return d; } catch (Exception) { }
                }
            }
            catch (Exception) { }
            Stopwatch sw = Stopwatch.StartNew();
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            List<string> one = new List<string>();
            one.Add(exeDir);
            try { one.Add(Path.GetDirectoryName(exeDir)); } catch (Exception) { }
            foreach (string sub in new string[] { "Desktop", "Downloads", "Documents", "" }) one.Add(Path.Combine(home, sub));
            one.Add(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles));
            one.Add(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86));
            foreach (string d in one) { string r = Look(d, 1, sw); if (r != null) return r; }
            List<string> roots = new List<string>();
            try
            {
                foreach (DriveInfo di in DriveInfo.GetDrives())
                    if (di.DriveType == DriveType.Fixed && di.IsReady) roots.Add(di.RootDirectory.FullName);
            }
            catch (Exception) { }
            roots.Sort(delegate(string a, string b) { return string.Compare(a, b, StringComparison.OrdinalIgnoreCase); });
            foreach (string d in roots) { string r = Look(d, 2, sw); if (r != null) return r; }
            return null;
        }

        public static bool IsDisc(string d)
        {
            try { return !string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, "PS3_GAME\\PARAM.SFO")); } catch (Exception) { return false; }
        }

        // The disc folder from RPCS3's game list: config\games.yml, lines like "BCES01143: <folder>/".
        public static string DetectDisc(string rpcs3)
        {
            try
            {
                if (string.IsNullOrEmpty(rpcs3)) return null;
                string yml = Path.Combine(rpcs3, "config\\games.yml");
                if (!File.Exists(yml)) return null;
                Dictionary<string, string> map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (string raw in File.ReadAllLines(yml))
                {
                    int c = raw.IndexOf(':');
                    if (c <= 0) continue;
                    string key = raw.Substring(0, c).Trim().Trim('"', '\'');
                    string val = raw.Substring(c + 1).Trim().Trim('"', '\'');
                    if (val.Length == 0) continue;
                    if (val.StartsWith("$(EmulatorDir)", StringComparison.OrdinalIgnoreCase)) val = Path.Combine(rpcs3, val.Substring(14).TrimStart('/', '\\'));
                    val = val.Replace('/', '\\').TrimEnd('\\');
                    if (!Path.IsPathRooted(val)) val = Path.Combine(rpcs3, val);
                    map[key] = val;
                }
                foreach (string id in Titles)
                {
                    string v;
                    if (map.TryGetValue(id, out v) && IsDisc(v)) return v;
                }
            }
            catch (Exception) { }
            return null;
        }

        // Forgiving game-folder pick: the PS3_GAME folder itself, or the folder one level above the disc.
        public static string NormalizeDisc(string p)
        {
            if (string.IsNullOrEmpty(p)) return p;
            try
            {
                p = p.TrimEnd('\\');
                if (string.Equals(Path.GetFileName(p), "PS3_GAME", StringComparison.OrdinalIgnoreCase)) return Path.GetDirectoryName(p);
                if (IsDisc(p)) return p;
                foreach (string id in Titles)
                    if (IsDisc(Path.Combine(p, id))) return Path.Combine(p, id);
                foreach (string d in Directory.GetDirectories(p))
                    if (IsDisc(d)) return d;
            }
            catch (Exception) { }
            return p;
        }

        public static bool IsAscii(string s)
        {
            foreach (char ch in s) if (ch > 126) return false;
            return true;
        }

        public static bool SamePath(string a, string b)
        {
            try { return string.Equals(Path.GetFullPath(a).TrimEnd('\\'), Path.GetFullPath(b).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase); }
            catch (Exception) { return false; }
        }

        public static bool EmptyDir(string d)
        {
            try { using (IEnumerator<string> en = Directory.EnumerateFileSystemEntries(d).GetEnumerator()) return !en.MoveNext(); }
            catch (Exception) { return false; }
        }

        public static void OpenUrl(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch (Exception) { }
        }

        public static void KillTree(int pid)
        {
            try
            {
                ProcessStartInfo si = new ProcessStartInfo("taskkill.exe", "/T /F /PID " + pid);
                si.UseShellExecute = false; si.CreateNoWindow = true;
                using (Process p = Process.Start(si)) p.WaitForExit(15000);
            }
            catch (Exception) { }
        }

        public static string Hash(string s)
        {
            uint h = 2166136261;
            foreach (char c in s) { h ^= c; h *= 16777619; }
            return h.ToString("x8");
        }

        // "[k/N] ..." as ninja prints it (N > 10, so cmake's "[0/2] Re-checking" is skipped).
        public static bool Ninja(string t, out int k, out int n)
        {
            k = n = 0;
            if (t.Length < 5 || t[0] != '[') return false;
            int i = 1;
            while (i < t.Length && char.IsDigit(t[i])) k = k * 10 + (t[i++] - '0');
            if (i == 1 || i >= t.Length || t[i] != '/') return false;
            int s = ++i;
            while (i < t.Length && char.IsDigit(t[i])) n = n * 10 + (t[i++] - '0');
            return i > s && i < t.Length && t[i] == ']' && n > 10 && k <= n;
        }

        // "... 45% ..." -> 45, else -1
        public static int Percent(string t)
        {
            int p = t.IndexOf('%');
            if (p < 1) return -1;
            int s = p;
            while (s > 0 && char.IsDigit(t[s - 1])) s--;
            int v;
            return p > s && int.TryParse(t.Substring(s, p - s), out v) ? v : -1;
        }

        // Run a hidden program and collect its output (used for the checks).
        public static int Capture(string file, string args, string dir, int timeoutMs, out string stdout, out string stderr)
        {
            ProcessStartInfo si = new ProcessStartInfo(file, args);
            si.UseShellExecute = false; si.CreateNoWindow = true;
            si.RedirectStandardOutput = true; si.RedirectStandardError = true; si.RedirectStandardInput = true;
            si.StandardOutputEncoding = Encoding.UTF8; si.StandardErrorEncoding = Encoding.UTF8;
            if (dir != null && Directory.Exists(dir)) si.WorkingDirectory = dir;
            using (Process p = Process.Start(si))
            {
                try { p.StandardInput.Close(); } catch (Exception) { }
                string o = "", e = "";
                Thread to = new Thread(delegate() { try { o = p.StandardOutput.ReadToEnd(); } catch (Exception) { } });
                Thread te = new Thread(delegate() { try { e = p.StandardError.ReadToEnd(); } catch (Exception) { } });
                to.IsBackground = true; te.IsBackground = true;
                to.Start(); te.Start();
                if (!p.WaitForExit(timeoutMs))
                {
                    KillTree(p.Id);
                    to.Join(3000); te.Join(3000);
                    stdout = o; stderr = "it did not answer within " + (timeoutMs / 1000) + " seconds";
                    return -1;
                }
                to.Join(10000); te.Join(10000);
                stdout = o; stderr = e;
                return p.ExitCode;
            }
        }
    }

    // ------------------------------------------------------------------ the checks

    sealed class Row
    {
        public bool? Ok;
        public string Name = "", Detail = "";
    }

    static class Checker
    {
        // The last Run could not start the scripts because Windows marked them as downloaded (error = BlockedHelp).
        public static volatile bool Blocked;

        // Files Setup unblocked itself (the ZIP was unpacked with Explorer): said once in the log.
        public static volatile int Unblocked;

        // Get-SetupChecks of tools\setup_common.ps1, as JSON (tools\setup_checks.ps1), in a hidden PowerShell.
        // Scripts refused for the "downloaded" mark: the port folder is unblocked and the checks run once more.
        public static List<Row> Run(string repo, string disc, string rpcs3, out string error)
        {
            List<Row> rows = RunOnce(repo, disc, rpcs3, out error);
            if (Blocked)
            {
                int n = Util.UnblockTree(repo);
                if (n > 0) { Unblocked += n; rows = RunOnce(repo, disc, rpcs3, out error); }
            }
            return rows;
        }

        static List<Row> RunOnce(string repo, string disc, string rpcs3, out string error)
        {
            error = null;
            Blocked = false;
            List<Row> rows = new List<Row>();
            try
            {
                string checks = Path.Combine(repo, "tools\\setup_checks.ps1");
                if (!File.Exists(checks)) { error = "tools\\setup_checks.ps1 is missing in " + repo; return rows; }
                string so, se;
                int code = Util.Capture(Util.PowerShellExe(), Util.PowerShellFile(checks, false) + " -Disc " + Util.Arg(disc ?? "") +
                                        " -Rpcs3 " + Util.Arg(rpcs3 ?? ""), repo, 180000, out so, out se);
                int a = so.IndexOf('['), b = so.IndexOf('{');
                int start = a >= 0 && (b < 0 || a < b) ? a : b;
                if (start < 0)
                {
                    if (Util.ScriptBlocked(se)) { Blocked = true; error = Util.BlockedHelp; return rows; }
                    string first = (se ?? "").Trim();
                    int nl = first.IndexOf('\n');
                    if (nl > 0) first = first.Substring(0, nl).Trim();
                    error = first.Length > 0 ? first : "PowerShell gave no answer (exit code " + code + ")";
                    return rows;
                }
                object o = new JavaScriptSerializer().DeserializeObject(so.Substring(start));
                object[] arr = o as object[];
                if (arr == null && o is Dictionary<string, object>) arr = new object[] { o };
                if (arr == null) { error = "the checker's answer could not be read"; return rows; }
                foreach (object x in arr)
                {
                    Dictionary<string, object> d = x as Dictionary<string, object>;
                    if (d == null) continue;
                    Row r = new Row();
                    object v;
                    if (d.TryGetValue("Ok", out v) && v is bool) r.Ok = (bool)v;
                    r.Name = Str(d, "Name");
                    r.Detail = Str(d, "Detail");
                    rows.Add(r);
                }
            }
            catch (Exception ex) { error = ex.Message; }
            return rows;
        }

        static string Str(Dictionary<string, object> d, string k)
        {
            object v;
            if (!d.TryGetValue(k, out v) || v == null) return "";
            object[] arr = v as object[];
            if (arr != null)
            {
                List<string> parts = new List<string>();
                foreach (object p in arr) parts.Add(Convert.ToString(p));
                return string.Join(" ", parts);
            }
            return Convert.ToString(v);
        }
    }

    // ------------------------------------------------------------------ running work

    // Work running in the background; its output lines are queued for the UI timer.
    interface IJob
    {
        ConcurrentQueue<string> Lines { get; }
        bool Exited { get; }
        int ExitCode { get; }
        void Kill();
    }

    // A hidden program, read on background threads (the window never waits). A segment ended by a
    // lone \r (a progress meter) is dropped: the scripts print whole lines.
    sealed class Job : IJob
    {
        readonly ConcurrentQueue<string> lines = new ConcurrentQueue<string>();
        volatile bool exited;
        int exitCode = -1;
        Process proc;

        public ConcurrentQueue<string> Lines { get { return lines; } }
        public bool Exited { get { return exited; } }
        public int ExitCode { get { return exitCode; } }

        public void Start(string file, string args, string dir, IDictionary<string, string> env)
        {
            ProcessStartInfo si = new ProcessStartInfo(file, args);
            si.UseShellExecute = false; si.CreateNoWindow = true;
            si.RedirectStandardOutput = true; si.RedirectStandardError = true; si.RedirectStandardInput = true;
            si.StandardOutputEncoding = Encoding.UTF8; si.StandardErrorEncoding = Encoding.UTF8;
            if (dir != null && Directory.Exists(dir)) si.WorkingDirectory = dir;
            if (env != null) foreach (KeyValuePair<string, string> kv in env) si.EnvironmentVariables[kv.Key] = kv.Value;
            proc = Process.Start(si);
            try { proc.StandardInput.Close(); } catch (Exception) { }   // nothing may wait for typed input
            Thread a = Pump(proc.StandardOutput), b = Pump(proc.StandardError);
            Thread w = new Thread(delegate()
            {
                a.Join(); b.Join();
                try { proc.WaitForExit(); exitCode = proc.ExitCode; } catch (Exception) { exitCode = -1; }
                exited = true;
            });
            w.IsBackground = true;
            w.Start();
        }

        Thread Pump(StreamReader r)
        {
            Thread t = new Thread(delegate()
            {
                char[] buf = new char[8192];
                StringBuilder sb = new StringBuilder();
                bool cr = false;
                try
                {
                    int n;
                    while ((n = r.Read(buf, 0, buf.Length)) > 0)
                    {
                        for (int i = 0; i < n; i++)
                        {
                            char c = buf[i];
                            if (cr)
                            {
                                cr = false;
                                if (c == '\n') { lines.Enqueue(sb.ToString()); sb.Length = 0; continue; }
                                sb.Length = 0;                       // "\r" alone: a meter redrawing itself
                            }
                            if (c == '\r') cr = true;
                            else if (c == '\n') { lines.Enqueue(sb.ToString()); sb.Length = 0; }
                            else sb.Append(c);
                        }
                    }
                }
                catch (Exception) { }
                if (sb.Length > 0) lines.Enqueue(sb.ToString());
            });
            t.IsBackground = true;
            t.Start();
            return t;
        }

        public void Kill()
        {
            try { if (proc != null && !proc.HasExited) Util.KillTree(proc.Id); } catch (Exception) { }
        }
    }

    // The port as one ZIP file from GitHub, no git: the port at the newest commit of main, with the
    // engine (engine\) and the port base (base\) in it (since v0.5.0; before, they were two more
    // downloads at the commits main pinned). It is unpacked next to its place and moved in when
    // complete, so a stopped download never leaves a half folder that looks whole. Lines:
    // "== [n/2] ...", "progress: <n>% ...", "error: <why>".
    sealed class PortDownload : IJob
    {
        const string Repo = "SamiEnazi/infamous2-port";
        readonly ConcurrentQueue<string> lines = new ConcurrentQueue<string>();
        volatile bool exited, cancel;
        int exitCode = -1;
        readonly string target;
        readonly bool update;           // the port is there already: the newest code over it, in place
        long doneBytes;
        const long TotalGuess = 40L << 20;      // the ZIP file (GitHub sends no size; v0.5.0's is about 40 MB)

        public PortDownload(string target, bool update) { this.target = target; this.update = update; }

        public ConcurrentQueue<string> Lines { get { return lines; } }
        public bool Exited { get { return exited; } }
        public int ExitCode { get { return exitCode; } }
        public void Kill() { cancel = true; }

        public void Start()
        {
            Thread t = new Thread(Run);
            t.IsBackground = true;
            t.Start();
        }

        void Say(string s) { lines.Enqueue(s); }

        void Run()
        {
            try
            {
                Say("== [1/2] asking GitHub for the newest version");
                string commit = Sha(Api("repos/" + Repo + "/commits/main"));
                Say("  port " + commit + " (with its engine and port base)");
                string have = update ? Util.ReadPortVersion(target) : null;
                bool pinned = update && Util.HadPins(target);   // (before port-version.txt is written again)
                if (have == commit && Util.PortComplete(target))
                {
                    if (pinned) DropOldLayout(true);
                    Say("  already the newest version: nothing to download");
                    Say("progress: 100% up to date");
                }
                else
                {
                    string temp = Path.Combine(Path.GetTempPath(), "inFAMOUS2-setup-download");
                    Directory.CreateDirectory(temp);
                    Part(2, "the port", Repo, commit, target, temp);
                    File.WriteAllText(Path.Combine(target, "port-version.txt"),
                        "Downloaded by Setup.exe " + DateTime.Now.ToString("yyyy-MM-dd HH:mm") + "\r\n" + Repo + " " + commit + "\r\n");
                    DropOldLayout(pinned);
                    try { Util.DeleteTree(temp); } catch (Exception) { }
                    Say("progress: 100% downloaded");
                }
                exitCode = 0;
            }
            catch (Exception ex)
            {
                Say("error: " + (cancel ? "stopped" : ex.Message));
                exitCode = 1;
            }
            exited = true;
        }

        // One part: the ZIP of repo at sha, unpacked into dest. Skipped when dest already has it.
        void Part(int step, string what, string repo, string sha, string dest, string temp)
        {
            Say("== [" + step + "/2] downloading " + what);
            string mark = Path.Combine(dest, ".zip-source");
            if (File.Exists(mark) && File.ReadAllText(mark).Trim() == repo + " " + sha) { Say("  " + what + ": already there"); return; }
            string zip = Path.Combine(temp, repo.Replace('/', '-') + ".zip");
            Download(Site("PORT_SETUP_CODELOAD", "https://codeload.github.com") + "/" + repo + "/zip/" + sha, zip, what);
            Say("progress: " + Pct() + "% unpacking " + what);
            string tmp = dest.TrimEnd('\\') + ".unzip";
            Util.DeleteTree(tmp);
            string tmpFull = Path.GetFullPath(tmp).TrimEnd('\\') + "\\";
            using (ZipArchive za = ZipFile.OpenRead(zip))
            {
                foreach (ZipArchiveEntry e in za.Entries)
                {
                    if (cancel) throw new OperationCanceledException();
                    string name = e.FullName.Replace('/', '\\');
                    int cut = name.IndexOf('\\');
                    if (cut < 0) continue;
                    name = name.Substring(cut + 1);                 // without the archive's top folder
                    if (name.Length == 0) continue;
                    string outPath = Path.GetFullPath(Path.Combine(tmp, name));
                    if (!outPath.StartsWith(tmpFull, StringComparison.OrdinalIgnoreCase)) throw new IOException("a bad path in the ZIP file: " + e.FullName);
                    if (name.EndsWith("\\")) { Directory.CreateDirectory(outPath); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(outPath));
                    e.ExtractToFile(outPath, true);
                }
            }
            File.WriteAllText(Path.Combine(tmp, ".zip-source"), repo + " " + sha);
            if (Directory.Exists(dest) && !Util.EmptyDir(dest))
            {
                if (!update) throw new IOException(dest + " is not empty");
                if (Util.SamePath(dest, target))
                {
                    // the engine and the port base: pure source, swapped whole; then the port's own files over the old ones
                    foreach (string s in Util.SwapTop)
                    {
                        string fresh = Path.Combine(tmp, s), have = Path.Combine(dest, s);
                        if (!Directory.Exists(fresh)) continue;
                        if (Directory.Exists(have) && !Util.EmptyDir(have)) Swap(fresh, have);
                        else
                        {
                            if (Directory.Exists(have)) Directory.Delete(have);
                            Directory.Move(fresh, have);
                        }
                        Say("  " + s + "\\: replaced whole");
                    }
                    MergeInto(tmp, dest);
                }
                else Swap(tmp, dest);
                File.Delete(zip);
                Say("  " + what + ": " + repo + " " + sha.Substring(0, 7));
                return;
            }
            if (Directory.Exists(dest)) Directory.Delete(dest);             // empty
            Directory.CreateDirectory(Path.GetDirectoryName(dest.TrimEnd('\\')));
            Directory.Move(tmp, dest);
            File.Delete(zip);
            Say("  " + what + ": " + repo + " " + sha.Substring(0, 7));
        }

        // The new port files over the old install, in place: each overwritten or added, nothing deleted,
        // never into the player's own folders (game\, work\, ...) and never through a link. The ".zip-source"
        // mark goes last, so a stopped update is done again next time.
        void MergeInto(string tmp, string dest)
        {
            string top = Path.GetFullPath(tmp).TrimEnd('\\');
            Dictionary<string, bool> seen = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            int n = 0, skipped = 0, same = 0;
            DateTime now = DateTime.UtcNow;
            foreach (string f in Directory.GetFiles(top, "*", SearchOption.AllDirectories))
            {
                if (cancel) throw new OperationCanceledException();
                string rel = f.Substring(top.Length + 1);
                if (Util.Protected(rel)) { skipped++; continue; }
                string dst = Path.Combine(dest, rel);
                string dir = Path.GetDirectoryName(dst);
                if (!Util.NoLinkBetween(dest, dir, seen)) { skipped++; Say("  kept as is (a link on the way): " + rel); continue; }
                Directory.CreateDirectory(dir);
                if (File.Exists(dst))
                {
                    FileAttributes a = File.GetAttributes(dst);
                    if ((a & FileAttributes.ReparsePoint) != 0) { skipped++; continue; }
                    if (Util.SameFile(f, dst)) { same++; continue; }   // unchanged: keeps its time
                    if ((a & FileAttributes.ReadOnly) != 0) File.SetAttributes(dst, a & ~FileAttributes.ReadOnly);
                }
                File.Copy(f, dst, true);
                // newer than anything built from the old file (the ZIP's time is the commit's, maybe older)
                File.SetLastWriteTimeUtc(dst, now);
                n++;
            }
            File.Copy(Path.Combine(top, ".zip-source"), Path.Combine(dest, ".zip-source"), true);
            Util.DeleteTree(top);
            Say("  " + n + " files updated in place, " + same + " unchanged, nothing deleted" + (skipped > 1 ? " (" + (skipped - 1) + " left alone)" : ""));
        }

        // Pure source, replaced whole: set aside first (nothing is lost if a move fails), then deleted;
        // never when a link (a junction or a symlink) is anywhere inside it.
        void Swap(string tmp, string dest)
        {
            string link = Util.FindLink(dest);
            if (link != null) throw new IOException("Setup will not replace " + dest + ": it contains a link (" + link + ")");
            Retime(tmp, dest);
            string old = dest.TrimEnd('\\') + ".old";
            Util.DeleteTree(old);
            Directory.Move(dest, old);
            Directory.Move(tmp, dest);
            try { Util.DeleteTree(old); } catch (Exception ex) { Say("  could not delete " + old + ": " + ex.Message); }
        }

        // toolkit\ and community\erikomis-infamous2-recomp\: the engine and the port base of an install from before
        // v0.5.0, or what a Setup.exe of v0.4.2 or older still downloads (the public repository keeps their two
        // pins for it). The port builds from engine\ and base\, so they are only left-overs: deleted when a
        // Setup download made them (their .zip-source mark) or the install's port-version.txt pinned them
        // (pinned: an older Setup.exe or a release zip), never with a link inside; else kept. Empty ones (the
        // placeholders a ZIP of the repository has for the pins) go too.
        void DropOldLayout(bool pinned)
        {
            string[] old = { "toolkit", "community\\erikomis-infamous2-recomp" };
            foreach (string o in old)
            {
                string d = Path.Combine(target, o);
                if (!Directory.Exists(d)) continue;
                try
                {
                    if (Util.EmptyDir(d)) { Directory.Delete(d); continue; }
                    if (!pinned && !File.Exists(Path.Combine(d, ".zip-source"))) { Say("  " + o + "\\ kept (not made by Setup): the port no longer uses it"); continue; }
                    Util.DeleteTree(d);
                    Say("  " + o + "\\ (before v0.5.0; the port now builds from engine\\ and base\\): deleted");
                }
                catch (Exception ex) { Say("  could not delete " + d + " (" + ex.Message + "): the port no longer uses it"); }
            }
            string cm = Path.Combine(target, "community");
            try { if (Directory.Exists(cm) && Util.EmptyDir(cm)) Directory.Delete(cm); } catch (Exception) { }
        }

        // Before a new tree replaces the old one: an unchanged file gets its old time back, a changed or new one
        // gets now (a ZIP's times are the commit's, maybe older than a build made from the old file). The build
        // files Setup keeps (tools\setup_cleanup.ps1 -KeepBuildCache) then recompile only what changed.
        void Retime(string fresh, string old)
        {
            string top = Path.GetFullPath(fresh).TrimEnd('\\');
            DateTime now = DateTime.UtcNow;
            int same = 0, changed = 0;
            foreach (string f in Directory.GetFiles(top, "*", SearchOption.AllDirectories))
            {
                if (cancel) throw new OperationCanceledException();
                string o = Path.Combine(old, f.Substring(top.Length + 1));
                try
                {
                    if (Util.SameFile(f, o)) { File.SetLastWriteTimeUtc(f, File.GetLastWriteTimeUtc(o)); same++; }
                    else { File.SetLastWriteTimeUtc(f, now); changed++; }
                }
                catch (Exception) { }
            }
            Say("  " + changed + " files new or changed, " + same + " unchanged");
        }

        int Pct() { return (int)Math.Min(99, 100 * doneBytes / TotalGuess); }

        // GitHub, or a stand-in for tests (PORT_SETUP_API, PORT_SETUP_CODELOAD: e.g. http://127.0.0.1:8765)
        static string Site(string env, string github)
        {
            string v = Environment.GetEnvironmentVariable(env);
            return string.IsNullOrEmpty(v) ? github : v.TrimEnd('/');
        }

        HttpWebRequest Request(string url)
        {
            HttpWebRequest r = (HttpWebRequest)WebRequest.Create(url);
            r.UserAgent = "inFAMOUS2-port-setup";
            r.Timeout = 60000; r.ReadWriteTimeout = 120000;
            return r;
        }

        // GitHub's API (no sign-in: 60 questions an hour per network, Setup asks 1)
        string Api(string path)
        {
            for (int attempt = 1; ; attempt++)
            {
                try
                {
                    HttpWebRequest r = Request(Site("PORT_SETUP_API", "https://api.github.com") + "/" + path);
                    r.Accept = "application/vnd.github+json";
                    using (WebResponse resp = r.GetResponse())
                    using (StreamReader sr = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                        return sr.ReadToEnd();
                }
                catch (WebException ex)
                {
                    HttpWebResponse hr = ex.Response as HttpWebResponse;
                    if (hr != null && (int)hr.StatusCode == 403)
                        throw new IOException("GitHub is limiting questions from this network right now. Wait a few minutes, then press Try again.");
                    if (attempt == 2 || cancel) throw new IOException("could not reach GitHub (" + ex.Message + "). Check the internet connection.");
                    Say("  GitHub: " + ex.Message + " -- trying once more");
                    Thread.Sleep(3000);
                }
            }
        }

        static string Sha(string json)
        {
            Dictionary<string, object> d = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
            object v;
            if (d == null || !d.TryGetValue("sha", out v) || !(v is string) || ((string)v).Length < 7) throw new IOException("GitHub's answer has no commit");
            return (string)v;
        }

        void Download(string url, string path, string what)
        {
            long start = doneBytes;
            for (int attempt = 1; ; attempt++)
            {
                try
                {
                    doneBytes = start;
                    HttpWebRequest r = Request(url);
                    using (WebResponse resp = r.GetResponse())
                    using (Stream s = resp.GetResponseStream())
                    using (FileStream f = File.Create(path))
                    {
                        byte[] buf = new byte[1 << 16];
                        int n;
                        long got = 0;
                        DateTime last = DateTime.MinValue;
                        while ((n = s.Read(buf, 0, buf.Length)) > 0)
                        {
                            if (cancel) throw new OperationCanceledException();
                            f.Write(buf, 0, n);
                            got += n; doneBytes += n;
                            if ((DateTime.Now - last).TotalMilliseconds > 400)
                            {
                                last = DateTime.Now;
                                Say(string.Format("progress: {0}% downloading {1} ({2:0.0} MB)", Pct(), what, got / 1048576.0));
                            }
                        }
                    }
                    return;
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception ex)
                {
                    if (attempt == 2 || cancel) throw new IOException("downloading " + what + " failed (" + ex.Message + "). Check the internet connection.");
                    Say("  " + what + ": " + ex.Message + " -- trying once more");
                    Thread.Sleep(3000);
                }
            }
        }
    }

    // Before Setup builds over an existing install: the player's saves, input.toml, config.local.json and
    // the game as it is now (exe + SDL2.dll), copied to %LOCALAPPDATA%\inFAMOUS2-port\save-backups\<time>
    // (not %TEMP%, which Windows may clean). The newest 3 backups are kept. Afterwards, built or not, the
    // saves are copied back, and the old game too if the new one was not built.
    sealed class SaveBackup
    {
        public readonly string Dir;
        public readonly int Files;
        readonly string repo;
        readonly DateTime takenUtc;
        static readonly string[] GameFiles = { "Infamous2Recomp.exe", "SDL2.dll" };

        static string SaveDir(string repo) { return Path.Combine(repo, "game\\gamedata\\dev_hdd0\\home\\00000001\\savedata"); }
        static string BuildDir(string repo) { return Path.Combine(repo, "work\\port\\build"); }

        public static string Root()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "inFAMOUS2-port\\save-backups");
        }

        public SaveBackup(string repo)
        {
            this.repo = repo;
            takenUtc = DateTime.UtcNow;
            string root = Root(), name = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            Dir = Path.Combine(root, name);
            for (int i = 2; Directory.Exists(Dir); i++) Dir = Path.Combine(root, name + "-" + i);
            Directory.CreateDirectory(Dir);
            Files = Util.CopyTree(SaveDir(repo), Path.Combine(Dir, "savedata"));
            foreach (string f in new string[] { "input.toml", "config.local.json" })
            {
                string p = Path.Combine(repo, f);
                if (File.Exists(p)) { File.Copy(p, Path.Combine(Dir, f), true); Files++; }
            }
            foreach (string f in GameFiles)
            {
                string p = Path.Combine(BuildDir(repo), f);
                if (!File.Exists(p)) continue;
                Directory.CreateDirectory(Path.Combine(Dir, "game"));
                File.Copy(p, Path.Combine(Dir, "game\\" + f), true);
            }
            // the newest 3 backups stay
            List<string> all = new List<string>(Directory.GetDirectories(root));
            all.Sort(StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < all.Count - 3; i++) { try { Util.DeleteTree(all[i]); } catch (Exception) { } }
        }

        // The new game was not built: the old one goes back (when it changed or went missing).
        public string RestoreGame()
        {
            int n = 0;
            foreach (string f in GameFiles)
            {
                string b = Path.Combine(Dir, "game\\" + f), live = Path.Combine(BuildDir(repo), f);
                if (!File.Exists(b)) continue;
                FileInfo bi = new FileInfo(b), li = new FileInfo(live);
                if (li.Exists && li.Length == bi.Length && li.LastWriteTimeUtc == bi.LastWriteTimeUtc) continue;
                Directory.CreateDirectory(BuildDir(repo));
                File.Copy(b, live, true);
                n++;
            }
            return n > 0 ? "the previous game is back (" + n + " file(s) from " + Path.Combine(Dir, "game") + ")" : "the previous game was not touched";
        }

        // The new game is built: the backup's copy of the old one is not needed.
        public void DropGame()
        {
            try { Util.DeleteTree(Path.Combine(Dir, "game")); } catch (Exception) { }
        }

        // The saves back over game\...\savedata: missing ones added, older ones overwritten with the backup.
        // A save written after the backup (the game played meanwhile) is newer and stays.
        public string Reapply()
        {
            string src = Path.Combine(Dir, "savedata"), dst = SaveDir(repo);
            int added = 0, back = 0, newer = 0;
            if (Directory.Exists(src))
            {
                foreach (string f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
                {
                    string rel = f.Substring(src.Length + 1), to = Path.Combine(dst, rel);
                    FileInfo ti = new FileInfo(to);
                    if (ti.Exists && ti.LastWriteTimeUtc > takenUtc) { newer++; continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(to));
                    if (ti.Exists && (ti.Attributes & FileAttributes.ReadOnly) != 0) File.SetAttributes(to, ti.Attributes & ~FileAttributes.ReadOnly);
                    File.Copy(f, to, true);
                    if (ti.Exists) back++; else added++;
                }
            }
            foreach (string f in new string[] { "input.toml", "config.local.json" })
            {
                string b = Path.Combine(Dir, f), live = Path.Combine(repo, f);
                if (File.Exists(b) && !File.Exists(live)) { File.Copy(b, live); added++; }
            }
            return "saves reapplied from " + Dir + ": " + added + " added, " + back + " overwritten" + (newer > 0 ? ", " + newer + " newer kept" : "");
        }
    }

    // ------------------------------------------------------------------ RecompLauncher: no window

    // recomplauncher.json starts Setup.exe in two ways. Only these switches come here; the window is untouched.
    //   Setup.exe --prepare --disc <game folder> (or --game) [--rpcs3 <folder>] [--dry-run]
    //     Easy Setup's "prepare" step: Install's checks and tools\setup_build.ps1 -AcceptMsvcLicense -Cleanup
    //     (the setup screen's label says that it accepts Microsoft's license), plus -NoShortcut (the launcher
    //     is the shortcut), without "Optimise for this PC" (its training run opens the game's window, and each
    //     launcher install is a new folder, so there is no earlier training to reuse): --pgo adds -Pgo
    //     (12 steps; the same exit codes and progress lines, "Training the build on your PC" while it plays).
    //     RPCS3 is found the way Install finds it. No window: "progress <0..1> <text>" lines on stdout, the
    //     whole log in play\setup.log. --dry-run stops before the build (nothing is written but the log).
    //   Setup.exe --play [play.ps1's options]
    //     Play: tools\play.ps1 without a console window (-Fullscreen added in Big Picture mode). Its lines go
    //     to play\launcher.log with the end of the session's stderr.log; ends with the game's own exit code.
    // Both put themselves and everything they start in a job that ends with them: when the launcher stops
    // Setup.exe, the build or the game stops too. recomplauncher.json's "exit_codes" explains each code.
    static class Headless
    {
        public const int Busy = 69, NoPort = 70, NotGameFolder = 71, NotThisGame = 72, NoRpcs3 = 73, NoFirmware = 74,
                         NoUpdate = 75, Rpcs3Open = 76, PcCannot = 77, BadDrive = 78, BuildFailed = 79,
                         NotBuilt = 80, PlayFailed = 82, ScriptsBlocked = 83;

        static StreamWriter stdout, stderr, log;
        static double lastFrac;

        public static int Run(Opts opt, string exeDir)
        {
            try
            {
                StayWithMe();
                return opt.Update ? Update(opt, exeDir) : opt.Prepare ? Prepare(opt, exeDir) : Play(opt, exeDir);
            }
            catch (Exception ex)
            {
                Write(ref stderr, true, "Setup.exe: " + ex.Message);
                Log("== stopped: " + ex);
                return opt.Prepare || opt.Update ? BuildFailed : PlayFailed;
            }
            finally
            {
                Native.KeepAwake(false);
                if (log != null) { try { log.Dispose(); } catch (Exception) { } log = null; }
            }
        }

        // stdout / stderr: the pipes the launcher reads (Stream.Null when nobody does)
        static void Write(ref StreamWriter w, bool err, string line)
        {
            try
            {
                if (w == null)
                {
                    w = new StreamWriter(err ? Console.OpenStandardError() : Console.OpenStandardOutput(), new UTF8Encoding(false));
                    w.AutoFlush = true;
                }
                w.WriteLine(line);
            }
            catch (Exception) { }
        }

        static void Say(string line) { Write(ref stdout, false, line); }

        static void Progress(double f, string text)
        {
            lastFrac = Math.Max(0, Math.Min(1, f));
            Say("progress " + lastFrac.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture) + " " + text);
        }

        static void OpenLog(string path)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                log = new StreamWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false));
                log.AutoFlush = true;
            }
            catch (Exception) { log = null; }
        }

        static void Log(string line)
        {
            if (log != null) { try { log.WriteLine(line); } catch (Exception) { } }
        }

        // The reason in the log, as the step's last line and on stderr; the code for the launcher.
        static int Stop(int code, string reason)
        {
            Log("== stopped (exit code " + code + "): " + reason);
            Progress(lastFrac, "Stopped: " + reason);
            Write(ref stderr, true, reason);
            return code;
        }

        // ---------------------------------------------------------- --prepare

        static int Prepare(Opts opt, string dir)
        {
            if (log == null) OpenLog(Path.Combine(dir, "play\\setup.log"));   // (--update --no-window: its own log goes on)
            Log(string.Format("Setup.exe --prepare{0} {1:yyyy-MM-dd HH:mm}, Windows {2}", opt.DryRun ? " --dry-run" : "", DateTime.Now, Environment.OSVersion.Version));
            // RecompLauncher installs each release as a new zip: no build files are kept for updates (setup_build.ps1)
            if (!opt.Update) Environment.SetEnvironmentVariable("PORT_BUILD_CACHE", "0");
            Progress(0, "Checking your PC and your game");
            bool first;
            using (Mutex m = new Mutex(true, "inFAMOUS2-port-setup-" + Util.Hash(dir.ToLowerInvariant()), out first))
            {
                if (!first) return Stop(Busy, "Setup is already running in " + dir);
                if (!Util.PortComplete(dir)) return Stop(NoPort, "the port's files are not next to Setup.exe in " + dir);
                string disc = Util.NormalizeDisc(opt.Disc);
                if (!Util.IsDisc(disc)) return Stop(NotGameFolder, "no PS3_GAME\\PARAM.SFO in " + (opt.Disc ?? "(no --disc given)"));
                string rpcs3 = string.IsNullOrEmpty(opt.Rpcs3) ? Util.FindRpcs3(dir, dir) : opt.Rpcs3;
                if (rpcs3 == null || !File.Exists(Path.Combine(rpcs3, "rpcs3.exe"))) return Stop(NoRpcs3, "RPCS3 was not found on this PC");
                Log("RPCS3: " + rpcs3);
                Log("game: " + disc);
                string err;
                int before = Checker.Unblocked;
                List<Row> rows = Checker.Run(dir, disc, rpcs3, out err);
                if (Checker.Unblocked > before) Log("unblocked " + (Checker.Unblocked - before) + " files Windows had marked as downloaded");
                if (err != null) return Stop(Checker.Blocked ? ScriptsBlocked : BuildFailed, "the checks could not run: " + err);
                foreach (Row r in rows) Log(string.Format("  {0,-5} {1}: {2}", r.Ok == true ? "ok" : r.Ok == false ? "MISS" : "note", r.Name, r.Detail));
                foreach (Row r in rows)
                    if (r.Ok == false) return Stop(CheckCode(r.Name), r.Name + ": " + r.Detail);
                if (Process.GetProcessesByName("rpcs3").Length > 0) return Stop(Rpcs3Open, "RPCS3 is open: close it (Setup uses it to unlock your game)");
                string cmd = Util.PowerShellFile(Path.Combine(dir, "tools\\setup_build.ps1"), false) +
                             " -Disc " + Util.Arg(disc) + " -Rpcs3 " + Util.Arg(rpcs3) + " -AcceptMsvcLicense -NoShortcut -Cleanup" +
                             (opt.Pgo ? " -Pgo" : "");
                Log("optimise for this PC: " + (opt.Pgo ? "yes (--pgo)" : "no"));
                if (opt.DryRun)
                {
                    Log("dry run, nothing built. The build would be: powershell " + cmd);
                    Progress(1, "Dry run: the checks passed, nothing was built");
                    return 0;
                }
                return Build(dir, cmd);
            }
        }

        // A MISS row of the checks (tools\setup_common.ps1 Get-SetupChecks) -> its exit code.
        static int CheckCode(string n)
        {
            if (n == "64-bit Windows" || n == "CPU with AVX2" || n == "Vulkan graphics driver") return PcCannot;
            if (n == "Game disc folder") return NotGameFolder;
            if (n.StartsWith("Game disc", StringComparison.Ordinal)) return NotThisGame;
            if (n == "Port folder's drive" || n == "Game folder's drive") return BadDrive;
            if (n.StartsWith("RPCS3", StringComparison.Ordinal)) return NoRpcs3;
            if (n == "PS3 firmware in RPCS3") return NoFirmware;
            if (n.StartsWith("Game update", StringComparison.Ordinal) || n.StartsWith("Digital game", StringComparison.Ordinal)) return NoUpdate;
            return BuildFailed;
        }

        static int Build(string dir, string cmd)
        {
            // over an existing game: the saves and the game as it is backed up first, as Install does
            SaveBackup backup = null;
            if (File.Exists(Util.BuiltExe(dir)))
            {
                try { backup = new SaveBackup(dir); Log("saves backed up to " + backup.Dir + " (" + backup.Files + " files, and the game as it is)"); }
                catch (Exception ex) { return Stop(BuildFailed, "Setup could not back up your saves (" + ex.Message + "), so it changed nothing"); }
            }
            Util.RefreshPath();
            Native.KeepAwake(true);
            Log("> setup_build.ps1");
            Job j = new Job();
            j.Start(Util.PowerShellExe(), cmd, dir, null);
            BuildProgress p = new BuildProgress(dir);
            string lastKey = null;
            DateTime lastAt = DateTime.MinValue;
            double shown = 0;
            while (true)
            {
                string ln;
                while (j.Lines.TryDequeue(out ln)) { Log(ln); p.Line(ln); }
                p.TailCompileLog();
                string key = p.Key();
                if (key != lastKey || (DateTime.Now - lastAt).TotalMilliseconds >= 1000)
                {
                    // never backwards: step 2 counts Python's own download, then all the other tools from 0%
                    shown = Math.Max(shown, p.Fraction());
                    Progress(shown, p.Text());
                    lastKey = key; lastAt = DateTime.Now;
                }
                if (j.Exited && j.Lines.IsEmpty) break;
                Thread.Sleep(250);
            }
            Log("[exit code " + j.ExitCode + "]");
            bool built = j.ExitCode == 0 && p.SawDone && File.Exists(Util.BuiltExe(dir));
            if (backup != null)
            {
                try { if (built) backup.DropGame(); else Log(backup.RestoreGame()); }
                catch (Exception ex) { Log("could not put the previous game back from " + backup.Dir + ": " + ex.Message); }
                try { Log(backup.Reapply()); }
                catch (Exception ex) { Log("could not reapply the saves from " + backup.Dir + ": " + ex.Message); }
            }
            if (!built) return Stop(BuildFailed, "the build stopped: " + (p.FailReason ?? "exit code " + j.ExitCode));
            // which port version this game was built from (--play notes it when an update brought another)
            try
            {
                string pv = Path.Combine(dir, "port-version.txt"), st = BuiltStamp(dir);
                if (File.Exists(pv)) File.Copy(pv, st, true);
                else if (File.Exists(st)) File.Delete(st);
            }
            catch (Exception) { }
            Log("== ready to play");
            Progress(1, "Ready to play");
            return 0;
        }

        static string BuiltStamp(string dir) { return Path.Combine(dir, "work\\launcher-built.txt"); }

        // setup_build.ps1's lines -> how far it is and one line for the player. The weights are the window's
        // (MainForm.StepWeight / StepWeightPgo, SubNames, SubWeight): 10 steps, or 12 with -Pgo (--pgo).
        sealed class BuildProgress
        {
            static readonly double[] StepWeight = { 0, 1, 22, 4, 2, 3, 10, 1, 52, 1, 4 };
            static readonly double[] StepWeightPgo = { 0, 1, 14, 3, 1, 2, 6, 1, 34, 19, 15, 1, 3 };
            static readonly string[] StepText = { "", "Checking your PC and your game", "Downloading the build tools",
                "Copying the update and the firmware files from RPCS3", "Unlocking your game with RPCS3", "Setting up the build tools",
                "Getting SDL2, the Vulkan headers and glslang", "Making the game's icon from your copy",
                "Translating and compiling the game (15-40 minutes)", "Almost done", "Cleaning up" };
            static readonly string[] StepTextPgo = { "", "Checking your PC and your game", "Downloading the build tools",
                "Copying the update and the firmware files from RPCS3", "Unlocking your game with RPCS3", "Setting up the build tools",
                "Getting SDL2, the Vulkan headers and glslang", "Making the game's icon from your copy",
                "Translating the game and compiling a training build", "Training the build on your PC (the game plays by itself, sound off)",
                "Compiling the game, optimised for this PC", "Almost done", "Cleaning up" };
            static readonly string[] SubNames = { "inputs", "port", "firmware", "ppu", "spu", "jobs", "compile" };
            static readonly double[] SubWeight = { 2, 3, 10, 22, 4, 9, 50 };
            static readonly string[] SubText = { "reading your game", "the port's code", "the firmware modules",
                "translating the game's code", "the SPU programs", "the SPU jobs", "compiling" };
            const int CompileStep = 8, PgoSteps = 12, FinalCompileStep = 10;

            int step, steps = 10, sub = -1, ninjaDone, ninjaTotal;
            double frac;
            string scriptText = "", detail = "", compileLog;
            long compilePos = -1;
            readonly string repo;
            public string FailReason;
            public bool SawDone;

            public BuildProgress(string dir) { repo = dir; compileLog = Path.Combine(dir, "work\\logs\\compile.log"); }

            bool Pgo() { return steps == PgoSteps; }
            double[] Weights() { return Pgo() ? StepWeightPgo : StepWeight; }
            bool Known() { return steps + 1 == Weights().Length; }
            // a step whose "[compile]" line and ninja lines in the compile log move the bar
            bool Compiling(int st) { return st == CompileStep || (Pgo() && st == FinalCompileStep); }

            public void Line(string line)
            {
                string t = line.Trim();
                int k, n;
                if (t.StartsWith("== [", StringComparison.Ordinal))
                {
                    int slash = t.IndexOf('/'), close = t.IndexOf(']');
                    if (slash > 4 && int.TryParse(t.Substring(4, slash - 4), out n)) step = n;
                    if (slash > 0 && close > slash && int.TryParse(t.Substring(slash + 1, close - slash - 1), out n)) steps = n;
                    scriptText = close > 0 ? t.Substring(close + 1).Trim() : t;
                    int so = scriptText.LastIndexOf("   (", StringComparison.Ordinal);
                    if (so > 0) scriptText = scriptText.Substring(0, so).Trim();
                    frac = 0; sub = -1; ninjaTotal = 0; detail = "";
                }
                else if (t.StartsWith("== FAILED:", StringComparison.Ordinal)) FailReason = t.Substring(10).Trim();
                else if (t.StartsWith("== done", StringComparison.Ordinal)) SawDone = true;
                else if (t.StartsWith("progress:", StringComparison.Ordinal))
                {
                    int pct = Util.Percent(t);
                    if (pct >= 0) frac = pct / 100.0;
                    detail = t.Substring(9).Trim();
                }
                else if (t.StartsWith("compile log:", StringComparison.Ordinal))
                {
                    string rel = t.Substring(12).Trim();
                    if (rel.Length > 0) { compileLog = Path.Combine(repo, rel); compilePos = -1; }
                }
                else if (Compiling(step) && t.Length > 2 && t[0] == '[' && t[t.Length - 1] == ']' &&
                         Array.IndexOf(SubNames, t.Substring(1, t.Length - 2)) >= 0)
                {
                    sub = Array.IndexOf(SubNames, t.Substring(1, t.Length - 2));
                    if (sub == SubNames.Length - 1)
                    {
                        ninjaTotal = 0;
                        try { compilePos = File.Exists(compileLog) ? new FileInfo(compileLog).Length : 0; } catch (Exception) { compilePos = 0; }
                    }
                }
                else if (Util.Ninja(t, out k, out n)) frac = k / (double)n;
            }

            // ninja's [k/N] lines in the compile log (the compile writes there, not to setup_build's output)
            public void TailCompileLog()
            {
                if (!Compiling(step) || sub != SubNames.Length - 1 || compilePos < 0) return;
                try
                {
                    using (FileStream fs = new FileStream(compileLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    {
                        if (fs.Length < compilePos) compilePos = 0;
                        if (fs.Length == compilePos) return;
                        fs.Seek(compilePos, SeekOrigin.Begin);
                        int want = (int)Math.Min(fs.Length - compilePos, 8 << 20), got = 0, r;
                        byte[] buf = new byte[want];
                        while (got < want && (r = fs.Read(buf, got, want - got)) > 0) got += r;
                        int last = got > 0 ? Array.LastIndexOf(buf, (byte)'\n', got - 1) : -1;
                        if (last < 0) { if (got == (8 << 20)) compilePos += got; return; }
                        int start = 0;
                        for (int i = 0; i <= last; i++)
                        {
                            if (buf[i] != '\n') continue;
                            if (buf[start] == '[' && i - start > 4)
                            {
                                int k, n;
                                if (Util.Ninja(Encoding.ASCII.GetString(buf, start, Math.Min(32, i - start)), out k, out n)) { ninjaDone = k; ninjaTotal = n; }
                            }
                            start = i + 1;
                        }
                        compilePos += last + 1;
                    }
                }
                catch (Exception) { }
            }

            public double Fraction()
            {
                if (step < 1) return 0;
                if (!Known()) return Math.Min(1, (step - 1 + Math.Min(1, frac)) / Math.Max(1, steps));
                double[] w = Weights();
                int st = Math.Min(step, steps);
                double done = 0;
                for (int i = 1; i < st; i++) done += w[i];
                double f = frac;
                if (Pgo() && st == FinalCompileStep)
                    f = sub == SubNames.Length - 1 && ninjaTotal > 0 ? ninjaDone / (double)ninjaTotal : 0;   // only "[compile]" here
                else if (st == CompileStep)
                {
                    f = 0;
                    if (sub >= 0)
                    {
                        for (int i = 0; i < sub; i++) f += SubWeight[i];
                        if (sub == SubNames.Length - 1 && ninjaTotal > 0) f += SubWeight[sub] * ninjaDone / (double)ninjaTotal;
                        f /= 100.0;
                    }
                }
                return (done + w[st] * Math.Min(1, f)) / 100.0;
            }

            public string Text()
            {
                if (step < 1) return "Getting ready";
                string[] tx = Pgo() ? StepTextPgo : StepText;
                string head = Known() && step < tx.Length ? tx[step] : scriptText;
                // with -Pgo, steps 8-10 say what they do this time (an update may compile with the last training)
                if (Known() && Pgo() && step >= CompileStep && step <= FinalCompileStep && scriptText.Length > 0)
                    head = char.ToUpperInvariant(scriptText[0]) + scriptText.Substring(1);
                if (Compiling(step) && sub >= 0)
                    return head + ": " + SubText[sub] + (sub == SubNames.Length - 1 && ninjaTotal > 0 ? " " + ninjaDone + " of " + ninjaTotal : "");
                return detail.Length > 0 ? head + ": " + detail : head;
            }

            // changes when the step or what it says changes (a new line is printed at once then)
            public string Key() { return step + "/" + sub + "/" + detail; }
        }

        // ---------------------------------------------------------- --update --no-window

        // Setup.exe --update --no-window --folder <port folder> [--disc <game folder>] [--rpcs3 <folder>] [--dry-run]
        //   What the window's "Update to the latest version" does, without the window (tests, scripts): the newest
        //   port over the folder in place (the player's files untouched; unchanged files keep their times, so kept
        //   build files rebuild only what changed), then the build as --prepare does it (saves backed up first and
        //   reapplied). The game folder: --disc, else game\disc (the link Setup made). --dry-run downloads and
        //   checks, then stops before the build. Log: play\update.log. Exit codes as --prepare's.
        static int Update(Opts opt, string exeDir)
        {
            string dir = Path.GetFullPath(string.IsNullOrEmpty(opt.Folder) ? exeDir : opt.Folder).TrimEnd('\\');
            OpenLog(Path.Combine(dir, "play\\update.log"));
            Log(string.Format("Setup.exe --update --no-window{0} {1:yyyy-MM-dd HH:mm}, Windows {2}{3}", opt.DryRun ? " --dry-run" : "",
                              DateTime.Now, Environment.OSVersion.Version, string.IsNullOrEmpty(opt.To) ? "" : ", to " + opt.To));
            Progress(0, "Getting the newest version");
            if (!Util.HasPort(dir)) return Stop(NoPort, "there is no port in " + dir + " to update");
            if (Directory.Exists(Path.Combine(dir, ".git")) || File.Exists(Path.Combine(dir, ".git")))
                return Stop(NoPort, "a git clone: git pull gets the newest code");
            bool first;
            using (Mutex m = new Mutex(true, "inFAMOUS2-port-setup-" + Util.Hash(dir.ToLowerInvariant()), out first))
            {
                if (!first) return Stop(Busy, "Setup is already running in " + dir);
                PortDownload d = new PortDownload(dir, true);
                d.Start();
                string err = null;
                while (true)
                {
                    string ln;
                    while (d.Lines.TryDequeue(out ln))
                    {
                        Log(ln);
                        if (ln.StartsWith("error:", StringComparison.Ordinal)) err = ln.Substring(6).Trim();
                        else if (ln.StartsWith("progress:", StringComparison.Ordinal))
                        {
                            int pct = Util.Percent(ln);
                            if (pct >= 0) Progress(0.02 * pct / 100.0, "Getting the newest version: " + ln.Substring(9).Trim());
                        }
                    }
                    if (d.Exited && d.Lines.IsEmpty) break;
                    Thread.Sleep(100);
                }
                if (d.ExitCode != 0 || !Util.PortComplete(dir)) return Stop(BuildFailed, "the download stopped: " + (err ?? "exit code " + d.ExitCode));
            }
            if (string.IsNullOrEmpty(opt.Disc) && Util.IsDisc(Path.Combine(dir, "game\\disc"))) opt.Disc = Path.Combine(dir, "game\\disc");
            return Prepare(opt, dir);   // (--dry-run: it stops before the build)
        }

        // ---------------------------------------------------------- --play

        static int Play(Opts opt, string exeDir)
        {
            string repo = Util.HasPort(exeDir) ? exeDir : Util.ExistingInstall(exeDir);
            if (repo == null) return Stop(NoPort, "the port's files are not next to Setup.exe in " + exeDir);
            string ps1 = Path.Combine(repo, "tools\\play.ps1");
            if (!File.Exists(ps1)) return Stop(NoPort, "tools\\play.ps1 is missing in " + repo);
            OpenLog(Path.Combine(repo, "play\\launcher.log"));
            Log(string.Format("Setup.exe --play {0:yyyy-MM-dd HH:mm}, Windows {1}, launcher {2} ({3})", DateTime.Now, Environment.OSVersion.Version,
                              Environment.GetEnvironmentVariable("RECOMPLAUNCHER_VERSION") ?? "-", Environment.GetEnvironmentVariable("RECOMPLAUNCHER_MODE") ?? "-"));
            // RecompLauncher updates this copy from the releases itself: play.ps1 then asks GitHub nothing (update_check.ps1)
            Environment.SetEnvironmentVariable("PS3_PORT_LAUNCHER", "recomplauncher");
            if (!File.Exists(Util.BuiltExe(repo))) return Stop(NotBuilt, "the game is not built yet (" + Util.BuiltExe(repo) + ")");
            // an update of the port arrived after this game was built: it still plays, the log says so
            try
            {
                string pv = Path.Combine(repo, "port-version.txt"), st = BuiltStamp(repo);
                if (File.Exists(pv) && File.Exists(st) && VersionLines(pv) != VersionLines(st))
                    Log("note: this game was built from an older version of the port than the one installed now (" + st + " vs " + pv +
                        "): setting it up again builds the new one");
            }
            catch (Exception) { }
            StringBuilder a = new StringBuilder(Util.PowerShellFile(ps1, true));
            bool fs = false;
            foreach (string s in opt.PlayArgs) if (string.Equals(s, "-Fullscreen", StringComparison.OrdinalIgnoreCase)) fs = true;
            if (!fs && string.Equals(Environment.GetEnvironmentVariable("RECOMPLAUNCHER_MODE"), "bigpicture", StringComparison.OrdinalIgnoreCase)) a.Append(" -Fullscreen");
            foreach (string s in opt.PlayArgs) a.Append(' ').Append(Arg(s));
            Log("> powershell " + a);
            if (opt.DryRun) { Log("dry run: the game was not started"); Say("dry run: powershell " + a); return 0; }

            Job j = new Job();
            try { j.Start(Util.PowerShellExe(), a.ToString(), repo, null); }
            catch (Exception ex) { return Stop(PlayFailed, "could not start PowerShell: " + ex.Message); }
            int code = 0;
            bool sawExit = false;
            string session = null;
            while (true)
            {
                string ln;
                while (j.Lines.TryDequeue(out ln))
                {
                    Log(ln);
                    Say(ln);
                    // play.ps1's last line: "Exited (code 0x<hex>) after <n> min. Logs: <session folder>"
                    int e = ln.IndexOf("Exited (code 0x", StringComparison.Ordinal);
                    if (e < 0) continue;
                    int h = e + 15, end = ln.IndexOf(')', h);
                    uint v;
                    if (end > h && uint.TryParse(ln.Substring(h, end - h), System.Globalization.NumberStyles.HexNumber, null, out v)) { code = unchecked((int)v); sawExit = true; }
                    int lg = ln.IndexOf("Logs: ", StringComparison.Ordinal);
                    if (lg > 0) session = ln.Substring(lg + 6).Trim();
                }
                if (j.Exited && j.Lines.IsEmpty) break;
                Thread.Sleep(200);
            }
            Log("[play.ps1 exit code " + j.ExitCode + "]");
            if (session != null) LogTail(Path.Combine(session, "stderr.log"), 150);
            if (sawExit) { Log("the game's exit code: " + code); return code; }
            return j.ExitCode == 0 ? 0 : Stop(PlayFailed, "play.ps1 ended with exit code " + j.ExitCode + " (see play\\launcher.log)");
        }

        // port-version.txt without its first line ("Downloaded by Setup.exe <time>" or the release's own)
        static string VersionLines(string path)
        {
            string[] l = File.ReadAllLines(path);
            return l.Length > 1 ? string.Join("\n", l, 1, l.Length - 1).Trim() : "";
        }

        // The last lines of the session's own log, for the crash report the launcher sends with launcher.log.
        static void LogTail(string path, int lines)
        {
            try
            {
                if (!File.Exists(path)) return;
                using (FileStream f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    long from = Math.Max(0, f.Length - (256 << 10));
                    f.Seek(from, SeekOrigin.Begin);
                    string[] all = new StreamReader(f, Encoding.UTF8).ReadToEnd().Split('\n');
                    int s = Math.Max(from > 0 ? 1 : 0, all.Length - lines);
                    Log("---- the end of " + path);
                    for (int i = s; i < all.Length; i++) Log(all[i].TrimEnd('\r'));
                }
            }
            catch (Exception) { }
        }

        static string Arg(string s) { return Util.Arg(s); }

        // ---------------------------------------------------------- a job that ends with Setup.exe

        [StructLayout(LayoutKind.Sequential)]
        struct BasicLimits
        {
            public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass, SchedulingClass;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct ExtendedLimits
        {
            public BasicLimits Basic;
            public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes;   // IO_COUNTERS
            public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
        }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr attributes, string name);
        [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimits info, int size);
        [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
        [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
        static IntPtr job;   // held until Setup.exe ends: then Windows ends everything in it

        static void StayWithMe()
        {
            try
            {
                job = CreateJobObject(IntPtr.Zero, null);
                if (job == IntPtr.Zero) return;
                ExtendedLimits l = new ExtendedLimits();
                l.Basic.LimitFlags = 0x2000;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                if (SetInformationJobObject(job, 9, ref l, Marshal.SizeOf(typeof(ExtendedLimits))))   // JobObjectExtendedLimitInformation
                    AssignProcessToJobObject(job, GetCurrentProcess());
            }
            catch (Exception) { }
        }
    }

    // ------------------------------------------------------------------ controls

    enum Kind { Primary, Secondary }

    // A big friendly rounded button that stays a real Button (keyboard, Enter, screen readers).
    class FlatBtn : Button
    {
        readonly Kind kind;
        bool hover, down;

        public FlatBtn(string text, Kind k, Font font)
        {
            kind = k; Text = text; Font = font;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw, true);
            FlatStyle = FlatStyle.Flat; FlatAppearance.BorderSize = 0; UseVisualStyleBackColor = false;
            UseMnemonic = false; Cursor = Cursors.Hand; ForeColor = Theme.Text;
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { down = true; Invalidate(); } base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Bg);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            bool cue = Focused && ShowFocusCues && Enabled;
            Color fill, border = Color.Empty, fore;
            if (kind == Kind.Primary)
            {
                fill = !Enabled ? Theme.Card : down ? Theme.AccentDown : hover ? Theme.AccentHover : Theme.Accent;
                fore = Enabled ? Theme.Ink : Theme.Dim;
            }
            else
            {
                fill = down ? Theme.SelBg : hover ? Theme.CardHi : Theme.Card;
                border = hover || cue ? Theme.Accent : Theme.Line;
                fore = hover ? Theme.AccentHi : Theme.Text;
            }
            RectangleF r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
            using (GraphicsPath p = Theme.Round(r, Theme.S(kind == Kind.Primary ? 12 : 7)))
            {
                using (SolidBrush b = new SolidBrush(fill)) g.FillPath(b, p);
                if (border != Color.Empty) using (Pen pen = new Pen(border, Math.Max(1f, Theme.Scale))) g.DrawPath(pen, p);
            }
            if (cue && kind == Kind.Primary)
            {
                float d = Theme.S(4);
                using (GraphicsPath p = Theme.Round(new RectangleF(d, d, Width - 2 * d - 1, Height - 2 * d - 1), Theme.S(9)))
                using (Pen pen = new Pen(Theme.Ink, Math.Max(1f, Theme.Scale)))
                    g.DrawPath(pen, p);
            }
            TextRenderer.DrawText(g, Text, Font, ClientRectangle, fore,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
        }
    }

    class Bar : Control, IAnimated
    {
        double frac;
        bool running;

        public Bar()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            SetStyle(ControlStyles.Selectable, false);
            TabStop = false;
            Anim.Add(this);
        }

        public double Value
        {
            get { return frac; }
            set
            {
                double v = Math.Max(0, Math.Min(1, value));
                if (Math.Abs(v - frac) > 0.0005) { frac = v; Invalidate(); }
            }
        }
        public bool Running { get { return running; } set { running = value; Invalidate(); } }
        public bool Animating { get { return running; } }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Bg);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float h = Height - 1f, rad = h / 2;
            RectangleF track = new RectangleF(0, 0, Width - 1f, h);
            using (GraphicsPath p = Theme.Round(track, rad))
            {
                using (SolidBrush b = new SolidBrush(Theme.Card)) g.FillPath(b, p);
                using (Pen pen = new Pen(Theme.Line)) g.DrawPath(pen, p);
            }
            float fw = (float)(track.Width * frac);
            if (frac > 0 && fw < h) fw = h;
            Color fill = Theme.Accent;
            if (fw > 0)
            {
                using (GraphicsPath p = Theme.Round(new RectangleF(0, 0, fw, h), rad))
                {
                    Color dark = Color.FromArgb(fill.R * 45 / 100, fill.G * 45 / 100, fill.B * 45 / 100);
                    using (LinearGradientBrush b = new LinearGradientBrush(new RectangleF(0, 0, Math.Max(2f, fw), h), dark, fill, LinearGradientMode.Horizontal))
                        g.FillPath(b, p);
                    if (running)
                    {
                        // a light band sweeping along the filled part: it is alive
                        float band = Math.Max(h * 4, 70 * Theme.Scale);
                        float x = (Anim.Frame * 7f * Theme.Scale) % (fw + band) - band;
                        RectangleF br = new RectangleF(x, 0, band, h);
                        using (LinearGradientBrush lb = new LinearGradientBrush(br, Color.Transparent, Color.Transparent, LinearGradientMode.Horizontal))
                        {
                            ColorBlend cb = new ColorBlend(3);
                            cb.Colors = new Color[] { Color.FromArgb(0, 255, 255, 255), Color.FromArgb(95, 255, 255, 255), Color.FromArgb(0, 255, 255, 255) };
                            cb.Positions = new float[] { 0f, 0.5f, 1f };
                            lb.InterpolationColors = cb;
                            Region old = g.Clip;
                            g.SetClip(p);
                            g.FillRectangle(lb, br);
                            g.Clip = old;
                            old.Dispose();
                        }
                    }
                }
            }
            else if (running)
            {
                // nothing done yet: a short block gliding along the track
                float bw = track.Width / 5;
                float x = (Anim.Frame * 6f * Theme.Scale) % (track.Width + bw) - bw;
                RectangleF br = RectangleF.Intersect(new RectangleF(x, 0, bw, h), track);
                if (br.Width > 2) using (GraphicsPath p = Theme.Round(br, rad)) using (SolidBrush b = new SolidBrush(fill)) g.FillPath(b, p);
            }
        }
    }

    // The logo, drawn smooth at any size.
    class Logo : Control
    {
        readonly Image img;

        public Logo(Image img)
        {
            this.img = img;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
            SetStyle(ControlStyles.Selectable, false);
            TabStop = false;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(Theme.Bg);
            if (img == null) return;
            e.Graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
            e.Graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            e.Graphics.DrawImage(img, ClientRectangle);
        }
    }

    // The one question Setup may ask: "Where is RPCS3?" / "Where is your inFAMOUS 2 game folder?"
    sealed class AskDialog : Form
    {
        public string Result;
        readonly bool rpcs3;
        readonly Label msg;

        public AskDialog(bool askRpcs3, string why, Icon icon)
        {
            rpcs3 = askRpcs3;
            Text = MainForm.Title;
            Icon = icon;
            AutoScaleMode = AutoScaleMode.None;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false; MinimizeBox = false; ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Bg; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = new Size(Theme.S(480), Theme.S(210));
            int w = ClientSize.Width - Theme.S(56);
            Label t = L(askRpcs3 ? "Where is RPCS3?" : "Where is your inFAMOUS 2 game folder?", Theme.H2, Theme.Text, Theme.S(26), w, Theme.S(30));
            Label s = L(why, Theme.Body, Theme.Dim, Theme.S(64), w, Theme.S(44));
            msg = L("", Theme.Small, Theme.Bad, Theme.S(110), w, Theme.S(22));
            FlatBtn browse = new FlatBtn("Browse...", Kind.Primary, Theme.Btn);
            browse.Size = new Size(Theme.S(150), Theme.S(42));
            browse.Location = new Point(Theme.S(28), Theme.S(146));
            FlatBtn cancel = new FlatBtn("Cancel", Kind.Secondary, Theme.Btn);
            cancel.Size = new Size(Theme.S(110), Theme.S(42));
            cancel.Location = new Point(browse.Right + Theme.S(12), browse.Top);
            Controls.AddRange(new Control[] { t, s, msg, browse, cancel });
            browse.Click += delegate { Browse(); };
            cancel.Click += delegate { DialogResult = DialogResult.Cancel; Close(); };
            AcceptButton = browse; CancelButton = cancel;
        }

        static Label L(string text, Font f, Color c, int y, int w, int h)
        {
            Label l = new Label();
            l.AutoSize = false; l.UseMnemonic = false; l.Font = f; l.ForeColor = c; l.Text = text;
            l.Location = new Point(Theme.S(28), y); l.Size = new Size(w, h);
            return l;
        }

        protected override void OnHandleCreated(EventArgs e) { base.OnHandleCreated(e); Native.DarkTitle(Handle, Theme.Bg); }

        void Browse()
        {
            string pick = null;
            if (rpcs3)
            {
                using (OpenFileDialog d = new OpenFileDialog())
                {
                    d.Title = "Find rpcs3.exe";
                    d.Filter = "RPCS3 (rpcs3.exe)|rpcs3.exe|Programs (*.exe)|*.exe";
                    if (d.ShowDialog(this) == DialogResult.OK) pick = Path.GetDirectoryName(d.FileName);
                }
                if (pick == null) return;
                if (!File.Exists(Path.Combine(pick, "rpcs3.exe"))) { msg.Text = "That folder has no rpcs3.exe."; return; }
            }
            else
            {
                using (FolderBrowserDialog d = new FolderBrowserDialog())
                {
                    d.Description = "Choose your inFAMOUS 2 game folder: the one that holds PS3_GAME.";
                    d.ShowNewFolderButton = false;
                    if (d.ShowDialog(this) == DialogResult.OK) pick = Util.NormalizeDisc(d.SelectedPath);
                }
                if (pick == null) return;
                if (!Util.IsDisc(pick)) { msg.Text = "That folder has no PS3_GAME folder in it."; return; }
            }
            Result = pick;
            DialogResult = DialogResult.OK;
            Close();
        }
    }

    // ------------------------------------------------------------------ the window

    sealed class MainForm : Form
    {
        public const string Title = "inFAMOUS 2 PC port - Setup";
        public const int BaseW = 640, BaseH = 500, LogH = 240;
        const string IssuesUrl = "https://github.com/SamiEnazi/infamous2-port/issues/new";
        const string MsvcLicense = "https://go.microsoft.com/fwlink/?LinkId=2179911";
        const string Intro = "Downloads the build tools it needs, builds the game from your copy, then cleans up.";

        enum State { Start, Working, Failed, Done }
        enum JobKind { None, Download, Build }

        readonly Opts opt;
        readonly string exeDir;
        string repo, target, rpcs3, disc;
        SaveBackup backup;               // set while Setup builds over an existing install
        bool updating, gitClone;
        string setupLog;
        string foundRpcs3;               // looked up in the background at start
        volatile bool searching;
        public int ExitCode;

        readonly FlatBtn big;
        readonly Bar bar;
        readonly Label status, line;
        readonly LinkLabel license, details, copyLog, again;
        readonly CheckBox pgoBox;        // "Optimise for this PC": setup_build.ps1 -Pgo
        readonly Label pgoNote;
        readonly TextBox logBox;
        readonly System.Windows.Forms.Timer pump, anim;
        bool showLog, forceClose;
        State state;

        IJob job;
        JobKind jobKind;
        StreamWriter logFile;
        readonly List<string> log = new List<string>();
        bool logDirty;
        // progress, from the job's lines
        int jStep, jSteps = 1, bSub = -1, ninjaDone, ninjaTotal;
        double jFrac, shownFrac;
        string failReason, compileLog, playerNote, pgoWhy;
        bool sawDone, pgoUsed;
        long compilePos = -1;

        static int S(double v) { return Theme.S(v); }

        public MainForm(Opts o, string dir)
        {
            opt = o; exeDir = dir;
            // Setup.exe in the port's folder, or downloaded on its own next to an earlier install; else
            // Install gets the port first
            repo = Util.HasPort(dir) ? dir : Util.ExistingInstall(dir);
            // --update --folder <port>: that folder (play.ps1 starts a Setup.exe it built in work\update\setup)
            if (o.Update && !string.IsNullOrEmpty(o.Folder) && Util.HasPort(o.Folder)) repo = Path.GetFullPath(o.Folder).TrimEnd('\\');
            ExitCode = o.TestBuild ? 2 : 0;

            Text = Title;
            Icon = Res.WindowIcon(dir);
            AutoScaleMode = AutoScaleMode.None;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Bg; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = new Size(S(BaseW), S(BaseH));
            int cw = ClientSize.Width;

            Logo logo = new Logo(Res.Logo(dir));
            logo.Size = new Size(S(92), S(92));
            logo.Location = new Point((cw - logo.Width) / 2, S(34));
            Label title = Center("inFAMOUS 2", Theme.H1, Theme.Text, S(132), S(50));
            Label sub = Center("PC port", Theme.Body, Theme.Accent, S(182), S(24));

            big = new FlatBtn("Install", Kind.Primary, Theme.BtnHuge);
            big.Size = new Size(S(330), S(76));
            big.Location = new Point((cw - big.Width) / 2, S(232));
            big.Name = "install";
            big.Click += delegate { OnBig(); };
            bar = new Bar();
            bar.Size = new Size(S(460), S(18));
            bar.Location = new Point((cw - bar.Width) / 2, S(252));
            bar.Visible = false;
            status = Center("", Theme.Bold, Theme.Text, S(282), S(28));
            line = Center(Intro, Theme.Body, Theme.Dim, S(322), S(70));
            license = Theme.Link("By installing you accept Microsoft's build tools license", Theme.Small);
            license.LinkArea = new LinkArea(license.Text.IndexOf("Microsoft"), license.Text.Length - license.Text.IndexOf("Microsoft"));
            license.LinkClicked += delegate { Util.OpenUrl(MsvcLicense); };
            license.Location = new Point((cw - license.PreferredWidth) / 2, S(434));

            // "Optimise for this PC" (profile-guided optimisation trained here): on unless it was turned
            // off for this folder before (work\setup-no-pgo) or --no-pgo
            pgoBox = new CheckBox();
            pgoBox.Text = "Optimise for this PC";
            pgoBox.Font = Theme.Body; pgoBox.ForeColor = Theme.Text; pgoBox.BackColor = Theme.Bg;
            pgoBox.FlatStyle = FlatStyle.Flat;
            pgoBox.FlatAppearance.BorderColor = Theme.Dim;
            pgoBox.FlatAppearance.CheckedBackColor = Theme.Bg;
            pgoBox.FlatAppearance.MouseOverBackColor = Theme.Bg;
            pgoBox.FlatAppearance.MouseDownBackColor = Theme.Bg;
            pgoBox.AutoSize = true; pgoBox.UseMnemonic = false; pgoBox.Cursor = Cursors.Hand;
            pgoBox.Checked = !o.NoPgo && !(repo != null && File.Exists(NoPgoMark(repo)));
            pgoNote = new Label();
            pgoNote.AutoSize = true; pgoNote.UseMnemonic = false; pgoNote.Font = Theme.Small; pgoNote.ForeColor = Theme.Dim;
            pgoNote.Text = "a faster game; adds 10–25 min: Setup plays it by itself once";
            int pw = pgoBox.PreferredSize.Width, nw = pgoNote.PreferredWidth, px = Math.Max(S(14), (cw - pw - S(6) - nw) / 2);
            pgoBox.Location = new Point(px, S(400));
            pgoNote.Location = new Point(px + pw + S(6), S(400) + (pgoBox.PreferredSize.Height - pgoNote.PreferredHeight) / 2);
            pgoNote.Click += delegate { pgoBox.Checked = !pgoBox.Checked; };

            details = Theme.Link("Details", Theme.Small);
            details.Location = new Point(S(18), S(BaseH) - S(32));
            details.LinkClicked += delegate { ToggleLog(); };
            copyLog = Theme.Link("Copy log", Theme.Small);
            copyLog.Location = new Point(details.Left + TextRenderer.MeasureText("Hide details", Theme.Small).Width + S(16), details.Top);
            copyLog.LinkClicked += delegate { CopyLog(); };
            copyLog.Visible = false;
            again = Theme.Link("Update to the latest version", Theme.Small);
            again.Location = new Point(cw - S(18) - again.PreferredWidth, details.Top);
            again.LinkClicked += delegate { Install(); };
            again.Visible = false;

            logBox = new TextBox();
            logBox.Multiline = true; logBox.ReadOnly = true; logBox.ScrollBars = ScrollBars.Vertical; logBox.WordWrap = true;
            logBox.BorderStyle = BorderStyle.None;
            logBox.BackColor = Theme.LogBg; logBox.ForeColor = Color.FromArgb(190, 200, 215); logBox.Font = Theme.Mono;
            logBox.Location = new Point(S(14), S(BaseH));
            logBox.Size = new Size(cw - S(28), S(LogH) - S(14));

            Controls.AddRange(new Control[] { logo, title, sub, big, bar, status, line, pgoBox, pgoNote, license, details, copyLog, again, logBox });
            AcceptButton = big;

            pump = new System.Windows.Forms.Timer();
            pump.Interval = 200;
            pump.Tick += delegate { OnPump(); };
            pump.Start();
            anim = new System.Windows.Forms.Timer();
            anim.Interval = 40;
            anim.Tick += delegate { Anim.Tick(); };
            anim.Start();
        }

        Label Center(string text, Font f, Color c, int y, int h)
        {
            Label l = new Label();
            l.AutoSize = false; l.UseMnemonic = false; l.Font = f; l.ForeColor = c; l.Text = text;
            l.TextAlign = ContentAlignment.MiddleCenter;
            l.Location = new Point(S(30), y); l.Size = new Size(ClientSize.Width - S(60), h);
            return l;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Native.DarkTitle(Handle, Theme.Bg);
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            bool built = repo != null && File.Exists(Util.BuiltExe(repo));
            if (opt.Page == "done" || (built && !opt.TestBuild)) SetState(State.Done);
            else SetState(State.Start);
            if (opt.Page == "done") return;
            if (opt.Page == "ask") { Ask(true, "Setup could not find RPCS3 on this PC. Show it where rpcs3.exe is."); return; }
            // look for RPCS3 now, so Install can start right away
            if (opt.Rpcs3 == null)
            {
                searching = true;
                string d = exeDir, r = repo;
                ThreadPool.QueueUserWorkItem(delegate { foundRpcs3 = Util.FindRpcs3(d, r); searching = false; });
            }
            if (opt.TestBuild) Install();
            // --update (F2 > Update in the game): "Update to the latest version", at once
            else if (opt.Update && repo != null) Install();
        }

        // ---------------------------------------------------------- the screen

        void SetState(State s)
        {
            state = s;
            bool work = s == State.Working;
            big.Visible = !work;
            big.Text = s == State.Done ? "Play" : s == State.Failed ? "Try again" : "Install";
            bar.Visible = work;
            bar.Running = work;
            status.Visible = work || s == State.Done;
            if (s == State.Done) { status.Text = ""; status.Visible = false; }
            line.Visible = s != State.Working;
            if (s == State.Start) { line.Text = Intro; line.ForeColor = Theme.Dim; }
            if (s == State.Done)
            {
                line.Text = (pgoUsed ? "Ready to play, optimised for this PC." : "Ready to play.") + PgoLine() +
                            (gitClone ? " (A git clone: git pull gets the newest code.)" : "");
                line.ForeColor = Theme.Dim;
            }
            license.Visible = s == State.Start || s == State.Failed;
            pgoBox.Visible = pgoNote.Visible = s == State.Start || s == State.Failed;
            copyLog.Visible = s == State.Failed;
            again.Visible = s == State.Done && !opt.TestBuild;
            Text = Title;
            if (!work && big.Visible) big.Focus();
        }

        void Status(string s) { status.Text = s; }

        // The one plain line when "Optimise for this PC" was on but could not be used (setup_build.ps1's
        // "  pgo: not used: <why>"): the game is the normal build, Setup did not fail.
        string PgoLine()
        {
            if (pgoUsed || string.IsNullOrEmpty(pgoWhy)) return "";
            return " Optimise for this PC didn't work this time (" + pgoWhy.TrimEnd('.') + "), so the game was built the normal way.";
        }

        // The "Optimise for this PC" choice, kept in the game folder for the next Install or Update.
        static string NoPgoMark(string r) { return Path.Combine(r, "work\\setup-no-pgo"); }

        void RememberPgo(bool on)
        {
            if (repo == null) return;
            string m = NoPgoMark(repo);
            try
            {
                if (on) { if (File.Exists(m)) File.Delete(m); }
                else { Directory.CreateDirectory(Path.GetDirectoryName(m)); File.WriteAllText(m, "Setup's \"Optimise for this PC\" was off\r\n"); }
            }
            catch (Exception) { }
        }

        void Fail(string reason)
        {
            if (backup != null)
            {
                FinishUpdate(false);
                reason += " Your saves and the game you had are kept.";
            }
            failReason = reason;
            Native.KeepAwake(false);
            SetState(State.Failed);
            line.Text = reason;
            line.ForeColor = Theme.Bad;
            AddLog("== stopped: " + reason);
            ExitCode = 1;
            if (opt.ExitWhenDone) CloseSoon();
        }

        void ToggleLog()
        {
            showLog = !showLog;
            ClientSize = new Size(ClientSize.Width, S(BaseH) + (showLog ? S(LogH) : 0));
            details.Text = showLog ? "Hide details" : "Details";
            logDirty = true;
            ShowLogTail();
        }

        void OnBig()
        {
            if (state == State.Done) Play();
            else Install();
        }

        // ---------------------------------------------------------- Install: everything, in order

        void Install()
        {
            if (job != null) return;
            failReason = null;
            SetState(State.Working);
            bar.Value = 0;
            Native.KeepAwake(true);
            if (repo == null)
            {
                target = string.IsNullOrEmpty(opt.Folder) ? Util.DefaultTarget(exeDir) : Path.GetFullPath(opt.Folder);
                string driveNote = string.IsNullOrEmpty(opt.Folder) ? Util.LinkNote(exeDir, target) : null;   // GitHub issue #2
                if (!Util.HasPort(target)) { BeginLog(null); if (driveNote != null) AddLog(driveNote); StartDownload(false); return; }   // a new install
                repo = target;
            }
            StartUpdate();
        }

        // The port is here already: the player's saves backed up, the newest code over the old (in place),
        // then the normal build; FinishUpdate puts the saves (and, if the build fails, the old game) back.
        void StartUpdate()
        {
            target = repo;
            BeginLog(null);
            Status("Backing up your saves\u2026");
            string root = repo;
            ThreadPool.QueueUserWorkItem(delegate          // a 130 MB copy: off the window's thread
            {
                SaveBackup b = null;
                string err = null;
                try { b = new SaveBackup(root); } catch (Exception ex) { err = ex.Message; }
                Post(delegate { BackedUp(b, err); });
            });
        }

        void BackedUp(SaveBackup b, string err)
        {
            if (b == null) { Fail("Setup could not back up your saves (" + err + "), so it changed nothing."); return; }
            backup = b;
            AddLog("saves backed up to " + backup.Dir + " (" + backup.Files + " files, and the game as it is)");
            gitClone = Directory.Exists(Path.Combine(repo, ".git")) || File.Exists(Path.Combine(repo, ".git"));
            if (gitClone)
            {
                // a git clone gets its code from git; Setup only rebuilds it
                AddLog("a git clone: the newest code comes with git pull");
                FindThenCheck();
                return;
            }
            StartDownload(true);
        }

        void StartDownload(bool update)
        {
            ResetProgress();
            updating = update;
            AddLog(update ? "> the newest port over " + target : "> download the port into " + target);
            Status(update ? "Getting the newest version\u2026" : "Downloading the game's build files\u2026");
            PortDownload d = new PortDownload(target, update);
            jobKind = JobKind.Download;
            job = d;
            d.Start();
        }

        // Built or not, after an update: the old game back if the new one was not built, the saves reapplied.
        void FinishUpdate(bool built)
        {
            if (backup == null) return;
            SaveBackup b = backup;
            backup = null;
            List<string> notes = new List<string>();
            try
            {
                if (built) b.DropGame();
                else notes.Add(b.RestoreGame());
            }
            catch (Exception ex) { notes.Add("could not put the previous game back from " + b.Dir + ": " + ex.Message); }
            try { notes.Add(b.Reapply()); }
            catch (Exception ex) { notes.Add("could not reapply the saves from " + b.Dir + ": " + ex.Message); }
            foreach (string n in notes)
            {
                AddLog(n);
                if (setupLog != null) { try { File.AppendAllText(setupLog, n + "\r\n"); } catch (Exception) { } }
            }
        }

        void DownloadEnded(int code)
        {
            if (code != 0 || !Util.PortComplete(target)) { Fail(failReason ?? "The download stopped. Check the internet connection, then press Try again."); return; }
            // keep a copy of Setup.exe in the game folder, for next time
            try
            {
                string dst = Path.Combine(target, "Setup.exe");
                if (!Util.SamePath(dst, Application.ExecutablePath)) File.Copy(Application.ExecutablePath, dst, true);
            }
            catch (Exception) { }
            repo = target;
            FindThenCheck();
        }

        // RPCS3 and the game: found, or asked for in one small dialog; then the checks.
        void FindThenCheck()
        {
            Status("Looking for RPCS3 and your game\u2026");
            bar.Value = 0;
            if (searching) { After(300, FindThenCheck); return; }
            string rp = opt.Rpcs3 ?? (rpcs3 != null && File.Exists(Path.Combine(rpcs3, "rpcs3.exe")) ? rpcs3 : null) ?? foundRpcs3;
            if (rp == null || !File.Exists(Path.Combine(rp, "rpcs3.exe")))
                rp = Ask(true, "Setup could not find RPCS3 on this PC. Show it where rpcs3.exe is.");
            if (rp == null) { Fail("Setup needs RPCS3 with your game in it. Press Try again when it is ready."); return; }
            rpcs3 = rp;
            string dc = opt.Disc ?? (Util.IsDisc(disc) ? disc : null) ?? Util.DetectDisc(rp);
            if (!Util.IsDisc(dc)) dc = Ask(false, "Setup could not find the game in RPCS3's game list. Show it the folder that holds PS3_GAME.");
            if (dc == null) { Fail("Setup needs your inFAMOUS 2 game folder (the one with PS3_GAME). Press Try again."); return; }
            disc = dc;
            AddLog("RPCS3: " + rpcs3);
            AddLog("game: " + disc);
            Status("Checking your PC and your game\u2026");
            string root = repo, d1 = disc, r1 = rpcs3;
            ThreadPool.QueueUserWorkItem(delegate
            {
                string err;
                int before = Checker.Unblocked;
                List<Row> rows = Checker.Run(root, d1, r1, out err);
                int unblocked = Checker.Unblocked - before;
                Post(delegate
                {
                    if (unblocked > 0) AddLog("Windows had marked " + unblocked + " of the port's files as downloaded from the internet: unblocked them");
                    Checked(rows, err);
                });
            });
        }

        string Ask(bool askRpcs3, string why)
        {
            using (AskDialog d = new AskDialog(askRpcs3, why, Icon))
                return d.ShowDialog(this) == DialogResult.OK ? d.Result : null;
        }

        // One line, and only for what the player must fix; a wrong folder is asked for again.
        void Checked(List<Row> rows, string err)
        {
            if (err != null) { Fail(Checker.Blocked ? err + "." : "Setup could not run its checks: " + err); return; }
            foreach (Row r in rows) AddLog(string.Format("  {0,-5} {1}: {2}", r.Ok == true ? "ok" : r.Ok == false ? "MISS" : "note", r.Name, r.Detail));
            foreach (Row r in rows)
            {
                if (r.Ok != false) continue;
                string n = r.Name;
                if (n == "64-bit Windows" || n == "CPU with AVX2" || n == "Vulkan graphics driver") { Fail("This PC can't run the game: " + r.Detail + "."); return; }
                if (n.StartsWith("Game disc", StringComparison.Ordinal))
                {
                    disc = Ask(false, "That folder is not inFAMOUS 2 (" + r.Detail + "). Show Setup the right game folder.");
                    if (disc == null) { Fail("Setup needs your inFAMOUS 2 game folder. Press Try again."); return; }
                    FindThenCheck();
                    return;
                }
                if (n.StartsWith("RPCS3", StringComparison.Ordinal))
                {
                    rpcs3 = Ask(true, "RPCS3 is not where Setup thought. Show it where rpcs3.exe is.");
                    if (rpcs3 == null) { Fail("Setup needs RPCS3 with your game in it. Press Try again when it is ready."); return; }
                    FindThenCheck();
                    return;
                }
                if (n == "PS3 firmware in RPCS3") { Fail("RPCS3 has no PS3 firmware yet. In RPCS3: File > Install Firmware, then press Try again."); return; }
                if (n == "Port folder's drive" || n == "Game folder's drive") { Fail(char.ToUpper(r.Detail[0]) + r.Detail.Substring(1) + "."); return; }   // GitHub issue #2
                if (n.StartsWith("Game update", StringComparison.Ordinal)) { Fail("The game's 1.04 update is not in RPCS3 yet. In RPCS3: File > Install Packages, then press Try again."); return; }
                Fail(n + ": " + r.Detail);
                return;
            }
            if (Process.GetProcessesByName("rpcs3").Length > 0) { Fail("Close RPCS3 (Setup uses it to unlock your game), then press Try again."); return; }
            StartBuild();
        }

        void StartBuild()
        {
            string script = Path.Combine(repo, "tools\\setup_build.ps1");
            Dictionary<string, string> env = null;
            if (opt.OnlyCache)
            {
                // test: only the downloaded tools; this PC's own installs are out of sight
                env = new Dictionary<string, string>();
                env["PORT_ONLY_CACHE"] = "1";
                env["Path"] = Util.WindowsPath();
            }
            else Util.RefreshPath();
            // pressing Install accepts Microsoft's license (the line under the button says so)
            bool pgo = pgoBox.Checked;
            RememberPgo(pgo);
            string cmd = Util.PowerShellFile(script, false) + " -Disc " + Util.Arg(disc) + " -Rpcs3 " + Util.Arg(rpcs3) +
                         (opt.TestFailBuild ? "" : " -AcceptMsvcLicense") + (opt.NoShortcut ? " -NoShortcut" : "") + (opt.NoCleanup ? "" : " -Cleanup") +
                         (pgo ? " -Pgo" : "");
            ResetProgress();
            compileLog = Path.Combine(repo, "work\\logs\\compile.log");
            Status("Getting ready\u2026");
            setupLog = Path.Combine(repo, "work\\setup.log");
            BeginLog(setupLog);
            if (opt.TestFailBuild) AddLog("(test: the build fails on purpose at step 2, without the license flag)");
            AddLog("RPCS3: " + rpcs3);
            AddLog("game: " + disc);
            AddLog("optimise for this PC: " + (pgo ? "yes (-Pgo)" : "no"));
            jobKind = JobKind.Build;
            AddLog("> setup_build.ps1" + (opt.OnlyCache ? " (PORT_ONLY_CACHE=1, Windows-only PATH)" : ""));
            Job j = new Job();
            try { j.Start(Util.PowerShellExe(), cmd, repo, env); }
            catch (Exception ex) { CloseLogFile(); Fail("Setup could not start PowerShell: " + ex.Message); return; }
            job = j;
        }

        // ---------------------------------------------------------- progress

        // How much of the whole build each step of setup_build.ps1 is (percent, on the test PC).
        static readonly double[] StepWeight = { 0, 1, 22, 4, 2, 3, 10, 1, 52, 1, 4 };
        // With "Optimise for this PC" (-Pgo, 12 steps): 8 = translate + the training build, 9 = the training
        // (the game plays by itself), 10 = the final compile.
        static readonly double[] StepWeightPgo = { 0, 1, 14, 3, 1, 2, 6, 1, 34, 19, 15, 1, 3 };
        static readonly double[] DownloadWeight = { 0, 3, 10, 72, 15 };
        // tools\rebuild_win.py's own steps inside step 8 ("[ppu]", ...), and their share of it.
        static readonly string[] SubNames = { "inputs", "port", "firmware", "ppu", "spu", "jobs", "compile" };
        static readonly double[] SubWeight = { 2, 3, 10, 22, 4, 9, 50 };
        const int ToolsStep = 2, CompileStep = 8, PgoSteps = 12, TrainStep = 9, FinalCompileStep = 10;

        bool PgoFlow() { return jSteps == PgoSteps; }
        // a step whose rebuild_win.py lines ("[compile]") and ninja lines in the compile log move the bar
        bool Compiling(int st) { return st == CompileStep || (PgoFlow() && st == FinalCompileStep); }

        void ResetProgress()
        {
            jStep = 0; jSteps = 1; jFrac = 0; bSub = -1; ninjaDone = ninjaTotal = 0; shownFrac = 0;
            failReason = null; sawDone = false; compilePos = -1; playerNote = null; pgoUsed = false; pgoWhy = null;
        }

        void ShowPlayerNote(string note)
        {
            playerNote = note;
            if (state != State.Working) return;
            line.Text = note ?? "";
            line.ForeColor = Theme.Dim;
            line.Visible = note != null;
        }

        void JobLine(string line)
        {
            AddLog(line);
            string t = line.Trim();
            int k, n;
            if (t.StartsWith("== [", StringComparison.Ordinal))
            {
                int slash = t.IndexOf('/'), close = t.IndexOf(']');
                if (slash > 4 && int.TryParse(t.Substring(4, slash - 4), out n)) { jStep = n; jFrac = 0; bSub = -1; ninjaTotal = 0; }
                if (slash > 0 && close > slash && int.TryParse(t.Substring(slash + 1, close - slash - 1), out n)) jSteps = n;
                if (playerNote != null) ShowPlayerNote(null);
            }
            else if (t.StartsWith("== FAILED:", StringComparison.Ordinal)) failReason = t.Substring(10).Trim();
            else if (t.StartsWith("== done", StringComparison.Ordinal)) sawDone = true;
            else if (t.StartsWith("error:", StringComparison.Ordinal)) failReason = t.Substring(6).Trim();
            else if (t.StartsWith("progress:", StringComparison.Ordinal)) { int pct = Util.Percent(t); if (pct >= 0) jFrac = pct / 100.0; }
            else if (jobKind == JobKind.Build && t.StartsWith("compile log:", StringComparison.Ordinal))
            {
                // setup_build.ps1 -Pgo: the training build logs to compile-build-train.log, the game to compile.log
                string rel = t.Substring(12).Trim();
                if (rel.Length > 0 && repo != null) { compileLog = Path.Combine(repo, rel); compilePos = -1; }
            }
            else if (jobKind == JobKind.Build && t.StartsWith("player-note:", StringComparison.Ordinal)) ShowPlayerNote(t.Substring(12).Trim());
            else if (jobKind == JobKind.Build && t.StartsWith("pgo: used", StringComparison.Ordinal)) pgoUsed = true;
            else if (jobKind == JobKind.Build && t.StartsWith("pgo: not used:", StringComparison.Ordinal)) pgoWhy = t.Substring(14).Trim();
            else if (jobKind == JobKind.Build && Compiling(jStep) && t.Length > 2 && t[0] == '[' && t[t.Length - 1] == ']' &&
                     Array.IndexOf(SubNames, t.Substring(1, t.Length - 2)) >= 0)
            {
                bSub = Array.IndexOf(SubNames, t.Substring(1, t.Length - 2));
                if (bSub == SubNames.Length - 1)
                {
                    // the compile writes to work\logs\compile.log: follow ninja's [k/N] there from here on
                    ninjaTotal = 0;
                    try { compilePos = File.Exists(compileLog) ? new FileInfo(compileLog).Length : 0; } catch (Exception) { compilePos = 0; }
                }
            }
            else if (Util.Ninja(t, out k, out n)) jFrac = k / (double)n;
        }

        void TailCompileLog()
        {
            if (!Compiling(jStep) || bSub != SubNames.Length - 1 || compilePos < 0) return;
            try
            {
                using (FileStream fs = new FileStream(compileLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    if (fs.Length < compilePos) compilePos = 0;
                    if (fs.Length == compilePos) return;
                    fs.Seek(compilePos, SeekOrigin.Begin);
                    int want = (int)Math.Min(fs.Length - compilePos, 8 << 20), got = 0, r;
                    byte[] buf = new byte[want];
                    while (got < want && (r = fs.Read(buf, got, want - got)) > 0) got += r;
                    int last = got > 0 ? Array.LastIndexOf(buf, (byte)'\n', got - 1) : -1;
                    if (last < 0) { if (got == (8 << 20)) compilePos += got; return; }
                    int start = 0;
                    for (int i = 0; i <= last; i++)
                    {
                        if (buf[i] != '\n') continue;
                        if (buf[start] == '[' && i - start > 4)
                        {
                            int k, n;
                            if (Util.Ninja(Encoding.ASCII.GetString(buf, start, Math.Min(32, i - start)), out k, out n)) { ninjaDone = k; ninjaTotal = n; }
                        }
                        start = i + 1;
                    }
                    compilePos += last + 1;
                }
            }
            catch (Exception) { }
        }

        double JobFraction()
        {
            double[] w = jobKind == JobKind.Download ? DownloadWeight : PgoFlow() ? StepWeightPgo : StepWeight;
            if (jStep < 1 || jSteps + 1 != w.Length) return 0;
            int st = Math.Min(jStep, jSteps);
            double done = 0;
            for (int i = 1; i < st; i++) done += w[i];
            double f = jFrac;
            if (jobKind == JobKind.Build && PgoFlow() && st == FinalCompileStep)
                f = bSub == SubNames.Length - 1 && ninjaTotal > 0 ? ninjaDone / (double)ninjaTotal : 0;   // only "[compile]" here
            else if (jobKind == JobKind.Build && st == CompileStep)
            {
                f = 0;
                if (bSub >= 0)
                {
                    double d = 0;
                    for (int j = 0; j < bSub; j++) d += SubWeight[j];
                    if (bSub == SubNames.Length - 1 && ninjaTotal > 0) d += SubWeight[bSub] * ninjaDone / (double)ninjaTotal;
                    f = d / 100.0;
                }
            }
            return (done + w[st] * Math.Min(1, f)) / 100.0;
        }

        // One short line: "Downloading tools... 45%", "Building... 3 of 7", "Cleaning up...".
        string StatusText()
        {
            if (jobKind == JobKind.Download) return (updating ? "Getting the newest version\u2026 " : "Downloading the game's build files\u2026 ") + (int)(JobFraction() * 100) + "%";
            if (jStep < ToolsStep) return "Getting ready\u2026";
            if (jStep == ToolsStep) return "Downloading tools\u2026 " + (int)(jFrac * 100) + "%";
            if (PgoFlow() && jStep == TrainStep && playerNote != null) return "Training the build on your PC\u2026 " + (int)(jFrac * 100) + "%";
            // the last step cleans up (10 of 10, or 12 of 12 with -Pgo); 3 .. last-1 build
            if (jStep < jSteps) return string.Format("Building\u2026 {0} of {1}", jStep - ToolsStep, jSteps - ToolsStep - 1);
            return "Cleaning up\u2026";
        }

        void OnPump()
        {
            if (job == null) return;
            IJob j = job;
            string ln;
            int count = 0;
            while (count < 4000 && j.Lines.TryDequeue(out ln)) { count++; JobLine(ln); }
            if (jobKind == JobKind.Build) TailCompileLog();
            double v = JobFraction();
            // never backwards in a build (with -Pgo, step 8 may compile twice: with the last training, then a training build)
            if (jobKind == JobKind.Build) { v = Math.Max(shownFrac, v); shownFrac = v; }
            bar.Value = v;
            status.Text = StatusText();
            if (jobKind == JobKind.Build) Text = (int)(v * 100) + "% - " + Title;
            ShowLogTail();
            if (j.Exited && j.Lines.IsEmpty)
            {
                job = null;
                AddLog("[exit code " + j.ExitCode + "]");
                CloseLogFile();
                ShowLogTail();
                if (jobKind == JobKind.Download) DownloadEnded(j.ExitCode);
                else BuildEnded(j.ExitCode);
            }
        }

        void BuildEnded(int code)
        {
            Native.KeepAwake(false);
            if (code == 0 && sawDone)
            {
                bar.Value = 1;
                ExitCode = 0;
                FinishUpdate(true);
                SetState(State.Done);
                if (opt.Update) line.Text = "Updated" + (string.IsNullOrEmpty(opt.To) ? "" : " to " + opt.To) + (pgoUsed ? ", optimised for this PC" : "") +
                                            ". Your saves, settings, mods and controls are kept." + PgoLine();
                if (opt.ExitWhenDone) CloseSoon();
                return;
            }
            Fail("The build stopped: " + (failReason ?? (code == 0 ? "it ended without finishing" : "exit code " + code)) +
                 ". Press Try again, or Copy log for a bug report.");
        }

        // ---------------------------------------------------------- the log (behind "Details")

        void BeginLog(string logPath)
        {
            CloseLogFile();
            if (logPath != null)
            {
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(logPath));
                    logFile = new StreamWriter(new FileStream(logPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false));
                    logFile.AutoFlush = true;
                    foreach (string l in log) logFile.WriteLine(l);
                }
                catch (Exception) { logFile = null; }
            }
            AddLog(string.Format("Setup.exe {0:yyyy-MM-dd HH:mm}, Windows {1}", DateTime.Now, Environment.OSVersion.Version));
        }

        void AddLog(string line)
        {
            log.Add(line);
            if (log.Count > 30000) log.RemoveRange(0, 10000);
            logDirty = true;
            if (logFile != null) { try { logFile.WriteLine(line); } catch (Exception) { } }
        }

        void CloseLogFile()
        {
            if (logFile == null) return;
            try { logFile.Dispose(); } catch (Exception) { }
            logFile = null;
        }

        // The last 200 lines (not while the player is reading in the box).
        void ShowLogTail()
        {
            if (!showLog || !logDirty || logBox.Focused) return;
            logDirty = false;
            int from = Math.Max(0, log.Count - 200);
            logBox.Text = string.Join("\r\n", log.GetRange(from, log.Count - from));
            logBox.SelectionStart = logBox.TextLength;
            logBox.ScrollToCaret();
        }

        void CopyLog()
        {
            string all = string.Join("\r\n", log);
            if (all.Length > 60000) all = "(the start is cut; the whole log is in work\\setup.log)\r\n" + all.Substring(all.Length - 60000);
            try
            {
                Clipboard.SetText(all.Length > 0 ? all : "(empty log)");
                copyLog.Text = "Copied: paste it into a bug report";
                copyLog.LinkArea = new LinkArea(0, copyLog.Text.Length);
                After(3000, delegate { copyLog.Text = "Copy log"; copyLog.LinkArea = new LinkArea(0, 8); });
                Util.OpenUrl(IssuesUrl);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not copy the log: " + ex.Message, Title, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        // ---------------------------------------------------------- play

        void Play()
        {
            if (repo == null) return;
            string ps1 = Path.Combine(repo, "tools\\play.ps1");
            if (!File.Exists(ps1)) { MessageBox.Show(this, "tools\\play.ps1 is missing in " + repo + ".", Title); return; }
            try
            {
                // the console starts minimized through the start info (as -WindowStyle Minimized did)
                ProcessStartInfo si = new ProcessStartInfo(Util.PowerShellExe(), Util.PowerShellFile(ps1, true));
                si.UseShellExecute = true;
                si.WindowStyle = ProcessWindowStyle.Minimized;
                si.WorkingDirectory = repo;
                Process.Start(si);
            }
            catch (Exception ex) { MessageBox.Show(this, "Could not start the game: " + ex.Message, Title); }
        }

        // ---------------------------------------------------------- small things

        void Post(MethodInvoker m)
        {
            try { if (IsHandleCreated && !IsDisposed) BeginInvoke(m); } catch (Exception) { }
        }

        void After(int ms, MethodInvoker m)
        {
            System.Windows.Forms.Timer t = new System.Windows.Forms.Timer();
            t.Interval = ms;
            t.Tick += delegate { t.Stop(); t.Dispose(); if (!IsDisposed) m(); };
            t.Start();
        }

        void CloseSoon() { After(2500, delegate { forceClose = true; Close(); }); }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (job != null && !job.Exited && !forceClose && e.CloseReason == CloseReason.UserClosing)
            {
                if (MessageBox.Show(this, "Setup is still working.\n\nStop it and close Setup?", Title, MessageBoxButtons.YesNo,
                                    MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.Yes)
                {
                    e.Cancel = true;
                    return;
                }
            }
            if (job != null && !job.Exited) job.Kill();
            FinishUpdate(false);       // stopped half-way: the old game and the saves back
            Native.KeepAwake(false);
            CloseLogFile();
            base.OnFormClosing(e);
        }
    }
}
