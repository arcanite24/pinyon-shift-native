// Shift Drive gameplay activity (SDL3 Vulkan): verifies the on-device play
// folder, binds Shift Engine Launcher settings.txt + Turnip driver + state/save
// paths, renders fullscreen in dark obsidian chrome with zero white flash, and
// provides a cutout-aware frosted launcher return pill for seamless return.
package net.shiftengine.drive;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.system.Os;
import android.util.TypedValue;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import org.libsdl.app.SDLActivity;

public class DriveActivity extends SDLActivity {
    public static final String EXTRA_OPEN_SETUP = "net.shiftengine.drive.OPEN_SETUP";
    public static final String EXTRA_FORCE_PLAY = "net.shiftengine.drive.FORCE_PLAY";
    public static final String EXTRA_BENCH_ARGS = "net.shiftengine.drive.BENCH_ARGS";
    public static final String EXTRA_DRIVER_OVERRIDE = "net.shiftengine.drive.DRIVER_OVERRIDE";
    public static final String EXTRA_TU_DEBUG = "net.shiftengine.drive.TU_DEBUG";
    public static final String EXTRA_ENV_OVERRIDES = "net.shiftengine.drive.ENV_OVERRIDES";

    private static final int COL_BG_OBSIDIAN = Color.parseColor("#0E1015");
    private static final int COL_PILL_BG = Color.parseColor("#D8161922");
    private static final int COL_PILL_BORDER = Color.parseColor("#B3E8195E");
    private static final int COL_PILL_TEXT = Color.parseColor("#ECEFF4");

    private boolean mRedirectingToSetup = false;
    private TextView mSetupPill = null;
    private int mTopCutoutInsetPx = 0;
    private final Handler mUiHandler = new Handler(Looper.getMainLooper());
    private final Runnable mHideSetupPill = () -> {
        if (mSetupPill != null) {
            mSetupPill.animate().alpha(0.0f).setDuration(300)
                    .withEndAction(() -> {
                        if (mSetupPill != null) mSetupPill.setVisibility(View.GONE);
                    }).start();
        }
    };

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        File root = getFilesRoot();
        File play = getPlayDirectory();
        File mods = getModsDirectory();
        if (play != null && mods != null) {
            List<String> args = new ArrayList<>();
            args.add("--play");
            args.add(play.getAbsolutePath());
            args.add("--mods");
            args.add(mods.getAbsolutePath());
            if (root != null) {
                File saveFile = LauncherSettingsStore.resolveActiveSaveFile(root);
                if (saveFile != null) {
                    File parent = saveFile.getParentFile();
                    if (parent != null) parent.mkdirs();
                    args.add("--save");
                    args.add(saveFile.getAbsolutePath());
                }
            }
            Intent launch = getIntent();
            if (launch != null) {
                String benchArgs = launch.getStringExtra(EXTRA_BENCH_ARGS);
                if (benchArgs == null) benchArgs = launch.getStringExtra("bench_args");
                if (benchArgs != null && !benchArgs.trim().isEmpty()) {
                    for (String tok : benchArgs.trim().split("\\s+")) {
                        if (!tok.isEmpty()) args.add(tok);
                    }
                }
            }
            return args.toArray(new String[0]);
        }
        return super.getArguments();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (getWindow() != null) {
            getWindow().setBackgroundDrawable(new ColorDrawable(COL_BG_OBSIDIAN));
        }
        configureEnvironment();
        configureBundledTurnipDriver();

        Intent launch = getIntent();
        boolean openSetup = launch != null && launch.getBooleanExtra(EXTRA_OPEN_SETUP, false);
        boolean forcePlay = launch != null && launch.getBooleanExtra(EXTRA_FORCE_PLAY, false);

        if (openSetup || (!forcePlay && !hasPlayFolder())) {
            mRedirectingToSetup = true;
            super.onCreate(savedInstanceState);
            returnToLauncher();
            return;
        }

        super.onCreate(savedInstanceState);
        if (getWindow() != null) {
            getWindow().setBackgroundDrawable(new ColorDrawable(COL_BG_OBSIDIAN));
            enableHdrColorModeIfSupported();
        }
        applyPreferredRefreshRate();
        enableImmersiveFullscreen();
        attachSetupCornerButton();
    }

    @Override
    protected void onResume() {
        super.onResume();
        enableImmersiveFullscreen();
    }

    @Override
    protected void onDestroy() {
        mUiHandler.removeCallbacksAndMessages(null);
        super.onDestroy();
    }

    @Override
    public void finish() {
        super.finish();
        LauncherSettingsStore.applyCrossFadeTransition(this, true);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            enableImmersiveFullscreen();
        }
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        if (ev != null && ev.getActionMasked() == MotionEvent.ACTION_DOWN && mSetupPill != null) {
            final float d = getResources().getDisplayMetrics().density;
            final int w = getWindow().getDecorView().getWidth();
            final float topBand = mTopCutoutInsetPx + 44f * d;
            if (ev.getY() < topBand && Math.abs(ev.getX() - 0.5f * w) < 104f * d) {
                revealSetupPillBriefly();
            }
        }
        return super.dispatchTouchEvent(ev);
    }

    private void returnToLauncher() {
        Intent launcher = new Intent(DriveActivity.this, SetupActivity.class);
        launcher.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(launcher);
        LauncherSettingsStore.applyCrossFadeTransition(this, false);
        finish();
    }

    @SuppressWarnings("deprecation")
    private void enableImmersiveFullscreen() {
        if (mRedirectingToSetup) return;
        try {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                WindowManager.LayoutParams lp = getWindow().getAttributes();
                lp.layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                getWindow().setAttributes(lp);
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                getWindow().setDecorFitsSystemWindows(false);
                WindowInsetsController ctrl = getWindow().getInsetsController();
                if (ctrl != null) {
                    ctrl.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                    ctrl.setSystemBarsBehavior(
                            WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                }
            } else {
                View decor = getWindow().getDecorView();
                decor.setSystemUiVisibility(
                        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_FULLSCREEN);
            }
        } catch (Throwable ignored) {
        }
    }

    private void revealSetupPillBriefly() {
        if (mSetupPill == null) return;
        mUiHandler.removeCallbacks(mHideSetupPill);
        mSetupPill.animate().cancel();
        mSetupPill.setVisibility(View.VISIBLE);
        mSetupPill.setAlpha(0.0f);
        mSetupPill.animate().alpha(0.78f).setDuration(160).start();
        mUiHandler.postDelayed(mHideSetupPill, 4200);
    }

    private void configureEnvironment() {
        try {
            File root = getFilesRoot();
            File play = getPlayDirectory();
            File state = getStateDirectory();
            File cache = getCacheDir();
            if (cache != null) {
                cache.mkdirs();
                Os.setenv("TMPDIR", cache.getAbsolutePath(), true);
            }
            if (root != null) {
                root.mkdirs();
                new File(root, "drive").mkdirs();
                File settingsFile = LauncherSettingsStore.settingsFile(root);
                Os.setenv("SHIFT_SETTINGS", settingsFile.getAbsolutePath(), true);
            }
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
    // via libadrenotools (internal filesDir staging + nativeLibraryDir hook libs).
    void configureBundledTurnipDriver() {
        try {
            String nativeLibDir = getApplicationInfo() != null ? getApplicationInfo().nativeLibraryDir : null;
            if (nativeLibDir != null && !nativeLibDir.isEmpty()) {
                if (!nativeLibDir.endsWith("/")) nativeLibDir += "/";
                Os.setenv("SHIFT_ANDROID_HOOK_LIB_DIR", nativeLibDir, true);
            }
            File tmpDir = new File(getCacheDir(), "adrenotools");
            tmpDir.mkdirs();
            Os.setenv("SHIFT_ANDROID_TMP_DIR", tmpDir.getAbsolutePath() + "/", true);

            String pref = "auto";
            String preset = "smooth-60";
            File root = getFilesRoot();
            if (root != null) {
                Map<String, String> settings = LauncherSettingsStore.readAll(
                        LauncherSettingsStore.settingsFile(root));
                pref = LauncherSettingsStore.getOrDefault(
                        settings, LauncherSettingsStore.KEY_GPU_DRIVER, "auto");
                preset = LauncherSettingsStore.getOrDefault(
                        settings, LauncherSettingsStore.KEY_PRESET, "smooth-60");
            }
            Intent launch = getIntent();
            boolean explicitTuDebug = false;
            if (launch != null) {
                String drvOverride = launch.getStringExtra(EXTRA_DRIVER_OVERRIDE);
                if (drvOverride == null) drvOverride = launch.getStringExtra("driver");
                if (drvOverride != null && !drvOverride.trim().isEmpty()) {
                    pref = drvOverride.trim();
                }
                String tuDebug = launch.getStringExtra(EXTRA_TU_DEBUG);
                if (tuDebug == null) tuDebug = launch.getStringExtra("tu_debug");
                if (tuDebug != null && !tuDebug.trim().isEmpty()) {
                    Os.setenv("TU_DEBUG", tuDebug.trim(), true);
                    explicitTuDebug = true;
                }
                String envOv = launch.getStringExtra(EXTRA_ENV_OVERRIDES);
                if (envOv == null) envOv = launch.getStringExtra("env");
                if (envOv != null && !envOv.trim().isEmpty()) {
                    for (String pair : envOv.split(";")) {
                        int eq = pair.indexOf('=');
                        if (eq > 0) {
                            String k = pair.substring(0, eq).trim();
                            String v = pair.substring(eq + 1).trim();
                            Os.setenv(k, v, true);
                            if ("TU_DEBUG".equals(k)) explicitTuDebug = true;
                        }
                    }
                }
            }

            // Enable Adreno GPU Turbo clocks except on Ultra Low / Low Power battery mode.
            boolean lowPower = "low-power".equals(preset) || "ultra-low".equals(preset) || "ultra-low-30".equals(preset);
            Os.setenv("SHIFT_ANDROID_GPU_TURBO", lowPower ? "0" : "1", true);

            if ("system".equalsIgnoreCase(pref)) {
                Os.setenv("SHIFT_ANDROID_GPU_DRIVER", "system", true);
                return;
            }
            // Direct/sysmem rendering on Adreno 7xx avoids Turnip's GMEM binning pass and
            // per-tile command replay across high-draw-count open-world driving scenes
            // (+30-50% FPS on Odin 2 Portal across 540p, 720p, 1080p, and 2x MSAA).
            if (!explicitTuDebug) {
                String existingTu = Os.getenv("TU_DEBUG");
                if (existingTu == null || existingTu.isEmpty()) {
                    Os.setenv("TU_DEBUG", "sysmem", true);
                }
            }

            // Stage Turnip into internal getFilesDir()/drivers/turnip/ because Android 10+
            // mounts external storage (/storage/emulated/0/...) noexec and libadrenotools
            // requires an app-private executable path.
            File internalDir = new File(getFilesDir(), "drivers/turnip");
            internalDir.mkdirs();
            File internalSo = new File(internalDir, "libvulkan_freedreno.so");

            File extSource = null;
            if (!"auto".equalsIgnoreCase(pref) && !"turnip".equalsIgnoreCase(pref)) {
                File explicit = new File(pref);
                if (explicit.isFile()) extSource = explicit;
            }
            if (extSource == null) {
                File state = getStateDirectory();
                if (state != null) {
                    File extDrivers = new File(state, "drivers");
                    File preferred = new File(extDrivers, "turnip/libvulkan_freedreno.so");
                    if (preferred.isFile()) {
                        extSource = preferred;
                    } else if (extDrivers.isDirectory()) {
                        File[] kids = extDrivers.listFiles();
                        if (kids != null) {
                            for (File k : kids) {
                                if (!k.isDirectory()) continue;
                                if (!k.getName().toLowerCase().contains("turnip")) continue;
                                File so = new File(k, "libvulkan_freedreno.so");
                                if (so.isFile()) {
                                    extSource = so;
                                    break;
                                }
                                File[] files = k.listFiles();
                                if (files != null) {
                                    for (File f : files) {
                                        if (f.isFile() && f.getName().endsWith(".so")) {
                                            extSource = f;
                                            break;
                                        }
                                    }
                                }
                                if (extSource != null) break;
                            }
                        }
                    }
                }
            }

            boolean staged = false;
            if (extSource != null && extSource.isFile()) {
                staged = syncFileIfChanged(extSource, internalSo);
            } else {
                staged = extractBundledAssetTurnipIfPresent(internalSo);
            }

            if (staged && internalSo.isFile()) {
                internalSo.setReadable(true, false);
                internalSo.setExecutable(true, false);
                Os.setenv("SHIFT_ANDROID_GPU_DRIVER", internalSo.getAbsolutePath(), true);
            }
        } catch (Throwable ignored) {
        }
    }

    private static boolean syncFileIfChanged(File src, File dst) {
        try {
            if (dst.isFile() && dst.length() == src.length() && dst.lastModified() >= src.lastModified()) {
                return true;
            }
            File parent = dst.getParentFile();
            if (parent != null) parent.mkdirs();
            File tmp = new File(parent, dst.getName() + ".tmp");
            try (FileInputStream in = new FileInputStream(src);
                 FileOutputStream out = new FileOutputStream(tmp)) {
                byte[] buf = new byte[131072];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                out.flush();
            }
            if (dst.exists()) dst.delete();
            if (!tmp.renameTo(dst)) return false;
            dst.setLastModified(src.lastModified());
            return true;
        } catch (Throwable ignored) {
            return dst.isFile();
        }
    }

    private boolean extractBundledAssetTurnipIfPresent(File dest) {
        try {
            long apkTime = 0L;
            try {
                apkTime = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            } catch (Throwable ignored) {
            }
            if (dest.isFile() && dest.length() > 0 && apkTime > 0L && dest.lastModified() >= apkTime) {
                return true;
            }
            try (InputStream in = getAssets().open("drivers/turnip/libvulkan_freedreno.so")) {
                File parent = dest.getParentFile();
                if (parent != null) parent.mkdirs();
                File tmp = new File(parent, dest.getName() + ".tmp");
                try (FileOutputStream out = new FileOutputStream(tmp)) {
                    byte[] buf = new byte[131072];
                    int n;
                    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                    out.flush();
                }
                if (dest.exists()) dest.delete();
                if (!tmp.renameTo(dest)) return false;
                if (apkTime > 0L) dest.setLastModified(apkTime);
                return true;
            }
        } catch (Throwable ignored) {
            return dest.isFile();
        }
    }

    // Requests HDR window color mode on Android HDR OLED displays (API 26+) so
    // Vulkan swapchain queries expose VK_COLOR_SPACE_HDR10_ST2084_EXT cleanly.
    private void enableHdrColorModeIfSupported() {
        if (mRedirectingToSetup || getWindow() == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        try {
            Display display = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    ? getDisplay()
                    : getWindowManager().getDefaultDisplay();
            if (display != null && display.isHdr()) {
                getWindow().setColorMode(ActivityInfo.COLOR_MODE_HDR);
            }
        } catch (Throwable ignored) {
        }
    }

    // Matches the display's hardware refresh mode (e.g. 120 Hz vs 60 Hz on Odin 2 Portal)
    // to the player's preset / FPS cap so Performance 120 runs at true 120 Hz and
    // 60/30 FPS presets save OLED power without frame-pacing judder.
    private void applyPreferredRefreshRate() {
        if (mRedirectingToSetup || getWindow() == null) return;
        try {
            File root = getFilesRoot();
            String preset = "smooth-60";
            String fpsCap = "60";
            if (root != null) {
                Map<String, String> settings = LauncherSettingsStore.readAll(
                        LauncherSettingsStore.settingsFile(root));
                preset = LauncherSettingsStore.getOrDefault(settings, LauncherSettingsStore.KEY_PRESET, "smooth-60");
                fpsCap = LauncherSettingsStore.getOrDefault(settings, LauncherSettingsStore.KEY_FPS_CAP, "60");
            }
            int desiredHz = 60;
            if ("performance-120".equals(preset) || "120".equals(fpsCap) || "display".equals(fpsCap)) {
                desiredHz = 120;
            } else if ("90".equals(fpsCap)) {
                desiredHz = 90;
            }
            setPreferredRefreshRateHz(desiredHz);
        } catch (Throwable ignored) {
        }
    }

    // Callable from native C++ via JNI when in-game FPS cap or preset changes at runtime.
    public void setPreferredRefreshRateHz(final int desiredHz) {
        if (mRedirectingToSetup) return;
        final float targetHz = (desiredHz >= 110) ? 120.0f : ((desiredHz >= 80) ? 90.0f : 60.0f);
        runOnUiThread(() -> {
            try {
                if (getWindow() == null) return;
                Display display = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                        ? getDisplay()
                        : getWindowManager().getDefaultDisplay();
                if (display == null) return;
                Display.Mode cur = display.getMode();
                Display.Mode[] modes = display.getSupportedModes();
                if (cur == null || modes == null || modes.length <= 1) return;
                Display.Mode best = cur;
                float bestDiff = Math.abs(cur.getRefreshRate() - targetHz);
                for (Display.Mode m : modes) {
                    if (m.getPhysicalWidth() != cur.getPhysicalWidth()
                            || m.getPhysicalHeight() != cur.getPhysicalHeight()) {
                        continue;
                    }
                    float diff = Math.abs(m.getRefreshRate() - targetHz);
                    if (diff < bestDiff) {
                        bestDiff = diff;
                        best = m;
                    }
                }
                if (best.getModeId() != cur.getModeId()) {
                    WindowManager.LayoutParams lp = getWindow().getAttributes();
                    lp.preferredDisplayModeId = best.getModeId();
                    getWindow().setAttributes(lp);
                }
            } catch (Throwable ignored) {
            }
        });
    }

    // Adds a subtle, non-focusable dark frosted capsule anchored in the
    // top-center safe bar (respecting display cutout insets) so players can
    // smoothly return to the Shift Engine Launcher without colliding with the
    // top-right HUD (position / timer / player bar), minimap, or touch pads.
    private void attachSetupCornerButton() {
        if (mRedirectingToSetup) return;
        try {
            final float d = getResources().getDisplayMetrics().density;
            TextView pill = new TextView(this);
            pill.setText("LAUNCHER");
            pill.setTextColor(COL_PILL_TEXT);
            pill.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f);
            pill.setTypeface(Typeface.DEFAULT_BOLD);
            pill.setLetterSpacing(0.08f);
            pill.setFocusable(false);
            pill.setFocusableInTouchMode(false);
            pill.setAlpha(0.78f);
            int px = Math.round(11 * d);
            int py = Math.round(4 * d);
            pill.setPadding(px, py, px, py);

            GradientDrawable bg = new GradientDrawable();
            bg.setColor(COL_PILL_BG);
            bg.setCornerRadius(14 * d);
            bg.setStroke(Math.max(1, Math.round(1 * d)), COL_PILL_BORDER);
            pill.setBackground(bg);
            pill.setOnClickListener(v -> returnToLauncher());

            final FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
            lp.topMargin = Math.round(6 * d);
            getWindow().addContentView(pill, lp);
            mSetupPill = pill;

            View decor = getWindow().getDecorView();
            if (decor != null) {
                decor.setOnApplyWindowInsetsListener((v, insets) -> {
                    int topCutout = 0;
                    if (insets != null) {
                        DisplayCutout cutout = insets.getDisplayCutout();
                        if (cutout != null) topCutout = cutout.getSafeInsetTop();
                    }
                    mTopCutoutInsetPx = topCutout;
                    if (mSetupPill != null && mSetupPill.getLayoutParams() instanceof FrameLayout.LayoutParams) {
                        FrameLayout.LayoutParams cur = (FrameLayout.LayoutParams) mSetupPill.getLayoutParams();
                        int desiredTop = topCutout + Math.round(6 * d);
                        if (cur.topMargin != desiredTop) {
                            cur.topMargin = desiredTop;
                            mSetupPill.setLayoutParams(cur);
                        }
                    }
                    return v.onApplyWindowInsets(insets);
                });
                decor.requestApplyInsets();
            }

            revealSetupPillBriefly();
        } catch (Throwable ignored) {
        }
    }

    public File getFilesRoot() {
        return LauncherSettingsStore.getInternalFilesRoot(this);
    }

    public File getPlayDirectory() {
        File root = LauncherSettingsStore.resolvePlayStorageRoot(this);
        return root != null ? new File(root, "play") : null;
    }

    public File getStateDirectory() {
        File root = getFilesRoot();
        return root != null ? new File(root, "state") : null;
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
