// JNI bridge wrapper exposing libmain.so's libs/setup & libs/profile C++
// routines to SetupActivity for 100% on-device ISO/STFS extraction, mod
// management, Turnip driver installation, save inspection/backup/import, and
// in-process world indexing.
package net.shiftengine.drive;

public final class SetupBridge {
    static {
        System.loadLibrary("main");
    }

    public interface ProgressListener {
        void onProgress(int stageIndex, int stageCount, float fraction, String label, String detail);
        void onLog(String line);
    }

    private SetupBridge() {}

    public static native String nativeInspectLibrary(String playDir, String stateDir);

    public static native String nativeExtractIsoFd(
            int fd, String outGameDir, boolean mediaOnly, boolean skipXex, ProgressListener listener);

    public static native String nativeInstallStfsFd(
            int fd, String displayName, String playDir, String stateDir, ProgressListener listener);

    public static native String nativeInstallModZipFd(
            int fd, String displayName, String modsDir, ProgressListener listener);

    public static native String nativeSetModEnabled(String modsDir, String modName, boolean enabled);

    public static native String nativeMoveMod(String modsDir, String modName, int delta);

    public static native String nativeRemoveMod(String modsDir, String modName);

    public static native String nativeInstallTurnipFd(int fd, String fileName, String driversDir);

    public static native String nativeRunOnDeviceBake(
            String gameDir, String stateDir, String playDir, boolean fullRoster, boolean resume,
            ProgressListener listener);

    public static native String nativeInspectSaves(String userDir);

    public static native String nativeBackupSave(String userDir);

    public static native String nativeImportSaveZipFd(
            int fd, String displayName, String userDir, String stateDir);

    public static native void nativeCancelSetup();
}
