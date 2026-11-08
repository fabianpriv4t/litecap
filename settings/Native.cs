using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace LiteCap.Settings;

/// <summary>Windows-Anmeldeinformationsverwaltung (gleiche Einträge wie der C++-Kern).</summary>
internal static class Credentials
{
    public const string ClientSecret = "LiteCap/YouTube/ClientSecret";
    public const string RefreshToken = "LiteCap/YouTube/RefreshToken";

    public static void Write(string target, string secret)
    {
        byte[] blob = Encoding.UTF8.GetBytes(secret);
        IntPtr mem = Marshal.AllocHGlobal(blob.Length);
        try
        {
            Marshal.Copy(blob, 0, mem, blob.Length);
            var c = new CREDENTIAL
            {
                Type = 1, // CRED_TYPE_GENERIC
                TargetName = target,
                CredentialBlobSize = (uint)blob.Length,
                CredentialBlob = mem,
                Persist = 2, // CRED_PERSIST_LOCAL_MACHINE
                UserName = "LiteCap",
            };
            if (!CredWrite(ref c, 0)) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        finally
        {
            // Klartext im Speicher überschreiben
            Marshal.Copy(new byte[blob.Length], 0, mem, blob.Length);
            Marshal.FreeHGlobal(mem);
            Array.Clear(blob);
        }
    }

    public static bool Exists(string target)
    {
        if (!CredRead(target, 1, 0, out IntPtr p)) return false;
        CredFree(p);
        return true;
    }

    public static void Delete(string target) => CredDelete(target, 1, 0);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct CREDENTIAL
    {
        public uint Flags;
        public uint Type;
        public string TargetName;
        public string? Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public string? TargetAlias;
        public string UserName;
    }

    [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredWrite(ref CREDENTIAL credential, uint flags);

    [DllImport("advapi32.dll", EntryPoint = "CredReadW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredRead(string target, uint type, uint flags, out IntPtr credential);

    [DllImport("advapi32.dll", EntryPoint = "CredDeleteW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredDelete(string target, uint type, uint flags);

    [DllImport("advapi32.dll")]
    private static extern void CredFree(IntPtr buffer);
}

/// <summary>Benachrichtigt den laufenden Kern (litecap.exe), die Konfiguration neu zu laden.</summary>
internal static class CoreWindow
{
    public static bool IsRunning => FindWindow("LiteCapCoreWindow", null) != IntPtr.Zero;

    public static void RequestReload()
    {
        IntPtr h = FindWindow("LiteCapCoreWindow", null);
        if (h != IntPtr.Zero) PostMessage(h, RegisterWindowMessage("LiteCap.ReloadConfig"), IntPtr.Zero, IntPtr.Zero);
    }

    [DllImport("user32.dll", EntryPoint = "FindWindowW", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindow(string? cls, string? title);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);

    [DllImport("user32.dll", EntryPoint = "RegisterWindowMessageW", CharSet = CharSet.Unicode)]
    private static extern uint RegisterWindowMessage(string name);
}
