// The Android app's activity (Slice 13B): SDL's activity with SDL linked
// statically into libmain.so, Turnip custom driver path hook, and LAN Wi-Fi
// sync support into Android/data/net.shiftengine.drive/files/play.
package net.shiftengine.drive;

import android.os.Bundle;
import android.system.Os;
import java.io.File;
import org.libsdl.app.SDLActivity;

public class DriveActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        configureBundledTurnipDriver();
        super.onCreate(savedInstanceState);
    }

    // Exposes a bundled or user-imported Turnip driver (.so) to Renderer::init()
    // via SHIFT_ANDROID_GPU_DRIVER when present under files/state/drivers/.
    private void configureBundledTurnipDriver() {
        try {
            File ext = getExternalFilesDir(null);
            if (ext == null) return;
            File drivers = new File(ext, "state/drivers");
            if (!drivers.isDirectory()) return;
            File[] kids = drivers.listFiles();
            if (kids == null) return;
            for (File k : kids) {
                if (!k.isDirectory()) continue;
                if (!k.getName().toLowerCase().contains("turnip")) continue;
                File so = new File(k, "libvulkan_freedreno.so");
                if (so.isFile()) {
                    Os.setenv("SHIFT_ANDROID_GPU_DRIVER", so.getAbsolutePath(), false);
                    return;
                }
            }
        } catch (Throwable ignored) {
        }
    }

    public File getPlayDirectory() {
        File ext = getExternalFilesDir(null);
        return ext != null ? new File(ext, "play") : null;
    }

    public boolean hasPlayFolder() {
        File play = getPlayDirectory();
        return play != null && new File(play, "play.txt").isFile();
    }
}
