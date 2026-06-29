using System;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Windows.Forms;
using System.Drawing;
using Microsoft.Win32;

// Background tray app: watches for the PS Vita UVC device and instantly opens
// the viewer (Edge app-mode, no browser chrome) when it's plugged in; closes
// it on unplug. Right-click the tray icon for options.
namespace VitaViewer
{
    class App : ApplicationContext
    {
        NotifyIcon tray;
        Process viewer;
        bool present = false;
        ManagementEventWatcher watcher;
        Timer poll;

        string ViewerHtml() { return Path.Combine(Application.StartupPath, "index.html"); }
        string UserData() { return Path.Combine(Path.GetTempPath(), "vitaviewer"); }

        public App()
        {
            tray = new NotifyIcon { Icon = SystemIcons.Application, Visible = true,
                                    Text = "Vita UVC Viewer - watching for Vita" };
            var menu = new ContextMenuStrip();
            menu.Items.Add("Open viewer now", null, (s, e) => Launch());
            var su = new ToolStripMenuItem("Start with Windows") { Checked = IsStartup() };
            su.Click += (s, e) => { SetStartup(!IsStartup()); su.Checked = IsStartup(); };
            menu.Items.Add(su);
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Quit", null, (s, e) => { Shutdown(); ExitThread(); });
            tray.ContextMenuStrip = menu;
            tray.DoubleClick += (s, e) => Launch();

            try
            {
                watcher = new ManagementEventWatcher(
                    new WqlEventQuery("SELECT * FROM Win32_DeviceChangeEvent"));
                watcher.EventArrived += (s, e) => Scan();
                watcher.Start();
            }
            catch { }

            poll = new Timer { Interval = 3000 };   // fallback poll
            poll.Tick += (s, e) => Scan();
            poll.Start();
            Scan();
        }

        bool VitaPresent()
        {
            try
            {
                using (var q = new ManagementObjectSearcher(
                    "SELECT Name FROM Win32_PnPEntity WHERE Name LIKE '%PSVita%' " +
                    "OR Name LIKE '%PS Vita%' OR PNPDeviceID LIKE '%PID_1337%'"))
                {
                    foreach (var o in q.Get()) return true;
                }
            }
            catch { }
            return false;
        }

        void Scan()
        {
            bool now = VitaPresent();
            if (now && !present) { present = true; Launch(); }
            else if (!now && present) { present = false; CloseViewer(); }
            tray.Text = present ? "Vita UVC Viewer - connected" : "Vita UVC Viewer - watching for Vita";
        }

        void Launch()
        {
            if (viewer != null && !viewer.HasExited) return;
            string edge = EdgePath();
            if (edge == null || !File.Exists(ViewerHtml())) return;
            var psi = new ProcessStartInfo { FileName = edge, UseShellExecute = false };
            psi.Arguments =
                "--app=\"file:///" + ViewerHtml().Replace('\\', '/') + "\" " +
                "--user-data-dir=\"" + UserData() + "\" --no-first-run " +
                "--disable-features=Translate --window-size=1280,720";
            try { viewer = Process.Start(psi); } catch { }
        }

        void CloseViewer()
        {
            try
            {
                if (viewer != null && !viewer.HasExited)
                {
                    // Kill the whole Edge instance (app-mode spawns children).
                    var k = new ProcessStartInfo("taskkill", "/PID " + viewer.Id + " /T /F")
                    { CreateNoWindow = true, UseShellExecute = false };
                    Process.Start(k).WaitForExit(2000);
                }
            }
            catch { }
            viewer = null;
        }

        void Shutdown()
        {
            CloseViewer();
            try { if (watcher != null) watcher.Stop(); } catch { }
            tray.Visible = false;
        }

        static string EdgePath()
        {
            string[] p = {
                @"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                @"C:\Program Files\Microsoft\Edge\Application\msedge.exe" };
            foreach (var x in p) if (File.Exists(x)) return x;
            return null;
        }

        static bool IsStartup()
        {
            using (var k = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Run"))
                return k != null && k.GetValue("VitaViewer") != null;
        }

        static void SetStartup(bool on)
        {
            using (var k = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Run", true))
            {
                if (on) k.SetValue("VitaViewer", "\"" + Application.ExecutablePath + "\"");
                else k.DeleteValue("VitaViewer", false);
            }
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.Run(new App());
        }
    }
}
