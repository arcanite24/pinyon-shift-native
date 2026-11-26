// Shift Drive gameplay activity (SDL3 Vulkan): verifies the on-device play
// folder, binds Shift Engine Launcher settings.txt + Turnip driver + state/save
// paths, renders fullscreen in dark obsidian chrome with zero white flash, and
// provides a cutout-aware frosted launcher return pill for seamless return.
package net.shiftengine.drive;

import android.content.Intent;
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
        }
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
    // via SHIFT_ANDROID_GPU_DRIVER when enabled by settings.txt (auto/turnip).
    void configureBundledTurnipDriver() {
        try {
            File root = getFilesRoot();
            if (root != null) {
                Map<String, String> settings = LauncherSettingsStore.readAll(
                        LauncherSettingsStore.settingsFile(root));
                String pref = LauncherSettingsStore.getOrDefault(
                        settings, LauncherSettingsStore.KEY_GPU_DRIVER, "auto");
                if ("system".equals(pref)) {
                    Os.setenv("SHIFT_ANDROID_GPU_DRIVER", "system", true);
                    return;
                }
            }
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
        File ext = getExternalFilesDir(null);
        return ext != null ? ext : getFilesDir();
    }

    public File getPlayDirectory() {
        File root = getFilesRoot();
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
