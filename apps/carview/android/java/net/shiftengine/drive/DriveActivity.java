// The Android app's launcher activity: verifies the on-device play folder
// (play.txt pointing to a valid game root with media/db/gamedb.slt +
// world/colorado.index), redirects to SetupActivity automatically when not
// ready, configures Turnip / state / mods environment paths, and launches
// Shift Drive under SDLActivity.
package net.shiftengine.drive;

import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.system.Os;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import org.libsdl.app.SDLActivity;

public class DriveActivity extends SDLActivity {
    public static final String EXTRA_OPEN_SETUP = "net.shiftengine.drive.OPEN_SETUP";
    public static final String EXTRA_FORCE_PLAY = "net.shiftengine.drive.FORCE_PLAY";

    private boolean mRedirectingToSetup = false;

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        File play = getPlayDirectory();
        File mods = getModsDirectory();
        if (play != null && mods != null) {
            return new String[] {
                "--play", play.getAbsolutePath(),
                "--mods", mods.getAbsolutePath()
            };
        }
        return super.getArguments();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        configureEnvironment();
        configureBundledTurnipDriver();

        Intent launch = getIntent();
        boolean openSetup = launch != null && launch.getBooleanExtra(EXTRA_OPEN_SETUP, false);
        boolean forcePlay = launch != null && launch.getBooleanExtra(EXTRA_FORCE_PLAY, false);

        if (openSetup || (!forcePlay && !hasPlayFolder())) {
            mRedirectingToSetup = true;
            super.onCreate(savedInstanceState);
            Intent setup = new Intent(this, SetupActivity.class);
            setup.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            startActivity(setup);
            finish();
            return;
        }

        super.onCreate(savedInstanceState);
        attachSetupCornerButton();
    }

    private void configureEnvironment() {
        try {
            File play = getPlayDirectory();
            File state = getStateDirectory();
            if (play != null) {
                play.mkdirs();
                Os.setenv("SHIFT_PLAY_DIR", play.getAbsolutePath(), true);
            }
            if (state != null) {
                state.mkdirs();
                new File(state, "mods").mkdirs();
                new File(state, "drivers").mkdirs();
                Os.setenv("SHIFT_STATE_ROOT", state.getAbsolutePath(), true);
            }
        } catch (Throwable ignored) {
        }
    }

    // Exposes a bundled or user-imported Turnip driver (.so) to Renderer::init()
    // via SHIFT_ANDROID_GPU_DRIVER when present under files/state/drivers/.
    void configureBundledTurnipDriver() {
        try {
            File state = getStateDirectory();
            if (state == null) return;
            File drivers = new File(state, "drivers");
            extractBundledAssetTurnipIfPresent(drivers);
            if (!drivers.isDirectory()) return;

            File preferred = new File(drivers, "turnip/libvulkan_freedreno.so");
            if (preferred.isFile()) {
                Os.setenv("SHIFT_ANDROID_GPU_DRIVER", preferred.getAbsolutePath(), true);
                return;
            }
            File[] kids = drivers.listFiles();
            if (kids == null) return;
            for (File k : kids) {
                if (!k.isDirectory()) continue;
                if (!k.getName().toLowerCase().contains("turnip")) continue;
                File so = new File(k, "libvulkan_freedreno.so");
                if (so.isFile()) {
                    Os.setenv("SHIFT_ANDROID_GPU_DRIVER", so.getAbsolutePath(), true);
                    return;
                }
            }
        } catch (Throwable ignored) {
        }
    }

    private void extractBundledAssetTurnipIfPresent(File driversDir) {
        File dest = new File(driversDir, "turnip/libvulkan_freedreno.so");
        if (dest.isFile()) return;
        try (InputStream in = getAssets().open("drivers/turnip/libvulkan_freedreno.so")) {
            File parent = dest.getParentFile();
            if (parent != null) parent.mkdirs();
            try (FileOutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
        } catch (Throwable ignored) {
        }
    }

    // Adds a subtle, non-focusable pill anchored in the top-center safe bar so
    // players can return to Setup anytime without colliding with the top-right
    // HUD (position / timer / player bar), minimap, speedometer, or touch pads.
    private void attachSetupCornerButton() {
        if (mRedirectingToSetup) return;
        try {
            final float d = getResources().getDisplayMetrics().density;
            TextView pill = new TextView(this);
            pill.setText("SETUP");
            pill.setTextColor(Color.parseColor("#E2E8F0"));
            pill.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f);
            pill.setFocusable(false);
            pill.setFocusableInTouchMode(false);
            pill.setAlpha(0.55f);
            int px = Math.round(9 * d);
            int py = Math.round(3 * d);
            pill.setPadding(px, py, px, py);

            GradientDrawable bg = new GradientDrawable();
            bg.setColor(Color.parseColor("#CC161B22"));
            bg.setCornerRadius(12 * d);
            bg.setStroke(Math.round(1 * d), Color.parseColor("#66F59E0B"));
            pill.setBackground(bg);
            pill.setOnClickListener(v -> {
                Intent setup = new Intent(DriveActivity.this, SetupActivity.class);
                startActivity(setup);
            });

            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
            lp.topMargin = Math.round(4 * d);
            getWindow().addContentView(pill, lp);
        } catch (Throwable ignored) {
        }
    }

    public File getPlayDirectory() {
        File ext = getExternalFilesDir(null);
        return ext != null ? new File(ext, "play") : null;
    }

    public File getStateDirectory() {
        File ext = getExternalFilesDir(null);
        return ext != null ? new File(ext, "state") : null;
    }

    public File getModsDirectory() {
        File state = getStateDirectory();
        return state != null ? new File(state, "mods") : null;
    }

    public boolean hasPlayFolder() {
        File play = getPlayDirectory();
        if (play == null) return false;
        File playTxt = new File(play, "play.txt");
        File worldIndex = new File(play, "world/colorado.index");
        if (!playTxt.isFile() || !worldIndex.isFile() || worldIndex.length() <= 0) {
            return false;
        }
        File gameRoot = resolveGameRootFromPlayTxt(play, playTxt);
        if (gameRoot == null) return false;
        return new File(gameRoot, "media/db/gamedb.slt").isFile()
                || new File(gameRoot, "Media/db/gamedb.slt").isFile();
    }

    private static File resolveGameRootFromPlayTxt(File playDir, File playTxt) {
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(playTxt), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                while (!line.isEmpty() && (line.endsWith("\r") || line.endsWith(" "))) {
                    line = line.substring(0, line.length() - 1);
                }
                if (line.startsWith("game ")) {
                    String path = line.substring(5).trim();
                    if (path.isEmpty()) return null;
                    File f = new File(path);
                    return f.isAbsolute() ? f : new File(playDir, path);
                }
            }
        } catch (Throwable ignored) {
        }
        return null;
    }
}
