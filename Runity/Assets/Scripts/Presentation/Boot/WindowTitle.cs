using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Runity.Presentation.Boot
{
    /// <summary>The game window's title bar text. Unity only offers the product name, which also names the log and settings folders,
    /// so on Windows the player's own window is renamed directly. In the Editor and on other platforms it does nothing.</summary>
    public static class WindowTitle
    {
        public static void Set(string title)
        {
#if UNITY_STANDALONE_WIN && !UNITY_EDITOR
            _title = title;
            _processId = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
            EnumWindows(RenameOwnWindow, IntPtr.Zero);
#endif
        }

#if UNITY_STANDALONE_WIN && !UNITY_EDITOR
        private static string _title;
        private static uint _processId;

        private delegate bool EnumWindowsProc(IntPtr window, IntPtr data);

        // Static with MonoPInvokeCallback so it also works under IL2CPP.
        [AOT.MonoPInvokeCallback(typeof(EnumWindowsProc))]
        private static bool RenameOwnWindow(IntPtr window, IntPtr data)
        {
            GetWindowThreadProcessId(window, out var owner);
            if (owner != _processId) return true;
            var className = new StringBuilder(64);
            GetClassName(window, className, className.Capacity);
            if (className.ToString() != "UnityWndClass") return true;
            SetWindowText(window, _title);
            return false;  // found: stop
        }

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr data);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr window, StringBuilder name, int capacity);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool SetWindowText(IntPtr window, string text);
#endif
    }
}
