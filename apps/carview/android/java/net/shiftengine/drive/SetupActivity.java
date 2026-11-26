// Shift Engine Android Launcher (SetupActivity): cohesive, handheld/console-grade
// launcher-first entry point for Shift Drive on Android.
// Features responsive Landscape rail + Portrait top-bar layouts, 6 tabs (Play,
// Setup, Content & Mods, Saves, Wi-Fi Sync, Settings & GPU Driver), cutout-aware
// insets, settings.txt live sync, full gamepad/D-pad navigation, and seamless
// cross-fade transitions into DriveActivity.
package net.shiftengine.drive;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ClipDrawable;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.Os;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.text.NumberFormat;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONArray;
import org.json.JSONObject;

public class SetupActivity extends Activity {
    private static final int REQ_PICK_ISO = 1001;
    private static final int REQ_PICK_FOLDER = 1002;
    private static final int REQ_PICK_STFS = 1003;
    private static final int REQ_PICK_MOD_ZIP = 1004;
    private static final int REQ_PICK_TURNIP = 1005;
    private static final int REQ_PICK_SAVE_ZIP = 1006;

    private static final int TAB_PLAY = 0;
    private static final int TAB_SETUP = 1;
    private static final int TAB_CONTENT = 2;
    private static final int TAB_SAVES = 3;
    private static final int TAB_WIFI = 4;
    private static final int TAB_SETTINGS = 5;
    private static final String[] TAB_NAMES = {
        "Play", "Setup", "Content", "Saves", "Wi-Fi Sync", "Settings"
    };

    // Shift Engine Launcher obsidian + Horizon crimson-magenta palette
    private static final int COL_CANVAS = Color.parseColor("#0E1015");
    private static final int COL_SURFACE = Color.parseColor("#151821");
    private static final int COL_CARD = Color.parseColor("#1B1F2B");
    private static final int COL_INSET = Color.parseColor("#12151D");
    private static final int COL_ROW_ALT = Color.parseColor("#212636");
    private static final int COL_BORDER = Color.parseColor("#2A3042");
    private static final int COL_TEXT_PRIMARY = Color.parseColor("#ECEFF4");
    private static final int COL_TEXT_SECONDARY = Color.parseColor("#9BA3B4");
    private static final int COL_TEXT_MUTED = Color.parseColor("#6C7486");

    private static final int COL_ACCENT = Color.parseColor("#E8195E");
    private static final int COL_ACCENT_FOCUS = Color.parseColor("#FF3B7F");
    private static final int COL_ACCENT_PRESS = Color.parseColor("#C91350");
    private static final int COL_ACCENT_SOFT = Color.parseColor("#28E8195E");

    private static final int COL_SUCCESS = Color.parseColor("#3FB96B");
    private static final int COL_SUCCESS_SOFT = Color.parseColor("#223FB96B");
    private static final int COL_WARNING = Color.parseColor("#D99B26");
    private static final int COL_WARNING_SOFT = Color.parseColor("#26D99B26");
    private static final int COL_DANGER = Color.parseColor("#E05252");
    private static final int COL_DANGER_SOFT = Color.parseColor("#2BE05252");

    private final ExecutorService mExecutor = Executors.newSingleThreadExecutor();
    private final AtomicBoolean mBusy = new AtomicBoolean(false);
    private final AtomicBoolean mCancelRequested = new AtomicBoolean(false);
    private final StringBuilder mSetupLogBuf = new StringBuilder();
    private final StringBuilder mWifiLogBuf = new StringBuilder();
    private final Map<String, String> mSettings = new LinkedHashMap<>();

    private int mActiveTab = TAB_PLAY;
    private boolean mIsLandscapeLayout = true;
    private boolean mLaunchingDrive = false;

    private Uri mPendingIsoUri = null;
    private String mPendingIsoName = null;
    private boolean mSetupLogVisible = false;
    private boolean mReadyToPlay = false;
    private boolean mHasGameDisc = false;
    private boolean mHasV4 = false;
    private boolean mHasTurnip = false;
    private String mTurnipPath = "";
    private int mDiscCarCount = 0;
    private int mDlcCount = 0;
    private int mModsEnabledCount = 0;
    private int mModsTotalCount = 0;
    private long mFreeStorageBytes = 0L;

    private JSONObject mLastLibraryJson = null;
    private JSONObject mLastSavesJson = null;
    private float mSetupProgressFrac = 0f;
    private String mSetupStageLabel = "Idle";
    private String mSetupStageDetail = "Select your game disc or run world indexing.";

    private String mWifiHostVal = "";
    private String mWifiPortVal = String.valueOf(WifiSyncClient.DISCOVERY_PORT);
    private String mWifiCodeVal = "";
    private float mWifiProgressFrac = 0f;
    private String mWifiStatusLabel = "Ready to pair";
    private String mWifiStatusDetail = "Open Shift Engine Launcher on PC -> Android Wi-Fi Sync.";

    // Shared chrome widgets
    private LinearLayout mRootContainer;
    private final List<View> mNavTabViews = new ArrayList<>();
    private final List<View> mNavTabIndicators = new ArrayList<>();
    private final List<TextView> mNavTabLabels = new ArrayList<>();
    private TextView mStatusPillLabel;
    private View mStatusPillDot;
    private TextView mStatusStorageLabel;
    private final FrameLayout[] mTabContainers = new FrameLayout[TAB_NAMES.length];

    // Tab 1: Play widgets
    private View mHeroStatusDot;
    private TextView mHeroStatusText;
    private TextView mHeroProfileText;
    private Button mBtnHeroPlay;
    private Button mBtnHeroGoSetup;
    private TextView mSummaryV4Val;
    private TextView mSummaryDlcVal;
    private TextView mSummaryModsVal;
    private TextView mSummaryDriverVal;
    private SegmentedControl mPlayPresetSeg;
    private SegmentedControl mPlayTouchSeg;
    private SegmentedControl mPlaySteerSeg;
    private SegmentedControl mPlayFpsSeg;

    // Tab 2: Setup widgets
    private TextView mDiscStatusText;
    private TextView mDiscHintText;
    private Button mBtnPickIso;
    private Button mBtnPickFolder;
    private Button mBtnRunSetup;
    private Button mBtnCancelSetup;
    private Button mBtnToggleSetupLog;
    private ProgressBar mSetupProgressBar;
    private TextView mSetupStageLabelView;
    private TextView mSetupStageDetailView;
    private ScrollView mSetupLogScroll;
    private TextView mSetupLogText;

    // Tab 3: Content & Mods widgets
    private Button mBtnAddStfs;
    private LinearLayout mPackagesList;
    private Button mBtnAddMod;
    private LinearLayout mModsList;

    // Tab 4: Saves widgets
    private TextView mSaveProfileTitle;
    private TextView mSaveMetaCar;
    private TextView mSaveMetaCredits;
    private TextView mSaveMetaGarage;
    private TextView mSaveMetaSigns;
    private TextView mSaveStatusBanner;
    private Button mBtnBackupSave;
    private Button mBtnImportSaveZip;
    private LinearLayout mBackupsList;

    // Tab 5: Wi-Fi Sync widgets
    private EditText mEditWifiHost;
    private EditText mEditWifiPort;
    private EditText mEditWifiCode;
    private Button mBtnWifiDiscover;
    private Button mBtnWifiSync;
    private Button mBtnWifiCancel;
    private ProgressBar mWifiProgressBar;
    private TextView mWifiStatusLabelView;
    private TextView mWifiStatusDetailView;
    private ScrollView mWifiLogScroll;
    private TextView mWifiLogText;

    // Tab 6: Settings & GPU Driver widgets
    private SegmentedControl mSetPresetSeg;
    private SegmentedControl mSetVsyncSeg;
    private SegmentedControl mSetFpsSeg;
    private SegmentedControl mSetTouchSeg;
    private SegmentedControl mSetSteerSeg;
    private SegmentedControl mSetDriverPrefSeg;
    private TextView mTurnipStatusText;
    private TextView mTurnipPathText;
    private Button mBtnImportTurnip;

    private final SetupBridge.ProgressListener mBridgeListener = new SetupBridge.ProgressListener() {
        @Override
        public void onProgress(int stageIndex, int stageCount, float fraction, String label, String detail) {
            runOnUiThread(() -> updateSetupProgressUi(fraction, label, detail));
        }

        @Override
        public void onLog(String line) {
            appendSetupLog(line);
        }
    };

    private static final class SegmentedControl {
        final Button[] buttons;
        final String[] values;
        int selectedIndex = 0;

        SegmentedControl(Button[] buttons, String[] values) {
            this.buttons = buttons;
            this.values = values;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (getWindow() != null) {
            getWindow().setBackgroundDrawable(new ColorDrawable(COL_CANVAS));
        }
        configureProcessEnv();
        ensureDirectories();
        loadSettingsFromDisk();
        mIsLandscapeLayout = shouldUseLandscapeLayout();
        buildUi();
        enableImmersiveChrome();
        appendSetupLog("Shift Engine Android Launcher ready.");
        appendSetupLog("Play directory: " + getPlayDir().getAbsolutePath());
        refreshAllAsync();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        mLaunchingDrive = false;
        loadSettingsFromDisk();
        syncSettingsControls();
        refreshAllAsync();
    }

    @Override
    protected void onResume() {
        super.onResume();
        mLaunchingDrive = false;
        enableImmersiveChrome();
        loadSettingsFromDisk();
        syncSettingsControls();
        refreshAllAsync();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) enableImmersiveChrome();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        captureWifiInputs();
        boolean wantLandscape = shouldUseLandscapeLayout();
        if (wantLandscape != mIsLandscapeLayout) {
            mIsLandscapeLayout = wantLandscape;
            buildUi();
            applyAllCachedStateToUi();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (mBusy.get()) {
            mCancelRequested.set(true);
            try {
                SetupBridge.nativeCancelSetup();
            } catch (Throwable ignored) {
            }
        }
        mExecutor.shutdownNow();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BUTTON_L1) {
            selectTab((mActiveTab + TAB_NAMES.length - 1) % TAB_NAMES.length);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_R1) {
            selectTab((mActiveTab + 1) % TAB_NAMES.length);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_START && mReadyToPlay && !mBusy.get()) {
            launchDriveWithAnimation();
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_Y && mActiveTab == TAB_SETUP) {
            toggleSetupLogVisibility();
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_B || keyCode == KeyEvent.KEYCODE_BACK) {
            if (mBusy.get()) {
                cancelActiveWork();
                return true;
            }
            if (mActiveTab == TAB_SETUP && mSetupLogVisible) {
                toggleSetupLogVisibility();
                return true;
            }
            if (mActiveTab != TAB_PLAY) {
                selectTab(TAB_PLAY);
                return true;
            }
        }
        return super.onKeyDown(keyCode, event);
    }

    private void configureProcessEnv() {
        try {
            File cache = getCacheDir();
            if (cache != null) {
                cache.mkdirs();
                Os.setenv("TMPDIR", cache.getAbsolutePath(), true);
            }
            Os.setenv("SHIFT_SETTINGS", LauncherSettingsStore.settingsFile(getFilesRoot()).getAbsolutePath(), true);
            Os.setenv("SHIFT_PLAY_DIR", getPlayDir().getAbsolutePath(), true);
            Os.setenv("SHIFT_STATE_ROOT", getStateDir().getAbsolutePath(), true);
        } catch (Throwable ignored) {
        }
    }

    @SuppressWarnings("deprecation")
    private void enableImmersiveChrome() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                WindowManager.LayoutParams lp = getWindow().getAttributes();
                lp.layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                getWindow().setAttributes(lp);
            }
            getWindow().setStatusBarColor(COL_CANVAS);
            getWindow().setNavigationBarColor(COL_CANVAS);
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

    private boolean shouldUseLandscapeLayout() {
        Configuration cfg = getResources().getConfiguration();
        if (cfg.orientation == Configuration.ORIENTATION_LANDSCAPE) return true;
        return cfg.screenWidthDp >= 640;
    }

    private File getFilesRoot() {
        File ext = getExternalFilesDir(null);
        return ext != null ? ext : getFilesDir();
    }

    private File getPlayDir() {
        return new File(getFilesRoot(), "play");
    }

    private File getGameDir() {
        return new File(getPlayDir(), "game");
    }

    private File getStateDir() {
        return new File(getFilesRoot(), "state");
    }

    private File getModsDir() {
        return new File(getStateDir(), "mods");
    }

    private File getDriversDir() {
        return new File(getStateDir(), "drivers");
    }

    private File getSettingsFile() {
        return LauncherSettingsStore.settingsFile(getFilesRoot());
    }

    private void ensureDirectories() {
        getPlayDir().mkdirs();
        getGameDir().mkdirs();
        getStateDir().mkdirs();
        getModsDir().mkdirs();
        getDriversDir().mkdirs();
        new File(getFilesRoot(), "drive").mkdirs();
    }

    private void loadSettingsFromDisk() {
        mSettings.clear();
        mSettings.putAll(LauncherSettingsStore.readAll(getSettingsFile()));
        if (!mSettings.containsKey(LauncherSettingsStore.KEY_PRESET)) {
            mSettings.put(LauncherSettingsStore.KEY_PRESET, "smooth-60");
        }
        if (!mSettings.containsKey(LauncherSettingsStore.KEY_TOUCH_CONTROLS)) {
            mSettings.put(LauncherSettingsStore.KEY_TOUCH_CONTROLS, "auto");
        }
        if (!mSettings.containsKey(LauncherSettingsStore.KEY_TOUCH_STEER)) {
            mSettings.put(LauncherSettingsStore.KEY_TOUCH_STEER, "buttons");
        }
        if (!mSettings.containsKey(LauncherSettingsStore.KEY_FPS_CAP)) {
            mSettings.put(LauncherSettingsStore.KEY_FPS_CAP, "60");
        }
        if (!mSettings.containsKey(LauncherSettingsStore.KEY_VSYNC)) {
            mSettings.put(LauncherSettingsStore.KEY_VSYNC, "fifo");
        }
        if (!mSettings.containsKey(LauncherSettingsStore.KEY_GPU_DRIVER)) {
            mSettings.put(LauncherSettingsStore.KEY_GPU_DRIVER, "auto");
        }
    }

    private void updateSetting(String key, String value) {
        mSettings.put(key, value);
        LauncherSettingsStore.writeAll(getSettingsFile(), mSettings);
        syncSettingsControls();
        updateDriverSummaryText();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    // ---- Drawable & Component Builders --------------------------------------

    private Drawable makeCardDrawable(int fillColor, int borderColor, int radiusDp) {
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(fillColor);
        gd.setCornerRadius(dp(radiusDp));
        gd.setStroke(Math.max(1, dp(1)), borderColor);
        return gd;
    }

    private Drawable makePillDrawable(int fillColor, int borderColor, int radiusDp) {
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(fillColor);
        gd.setCornerRadius(dp(radiusDp));
        if (borderColor != 0) gd.setStroke(Math.max(1, dp(1)), borderColor);
        return gd;
    }

    private Drawable makeCircleDrawable(int color) {
        GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.OVAL);
        gd.setColor(color);
        return gd;
    }

    private Drawable makeButtonDrawable(int normalBg, int focusBg, int pressBg, int borderCol, int radiusDp) {
        GradientDrawable normal = new GradientDrawable();
        normal.setColor(normalBg);
        normal.setCornerRadius(dp(radiusDp));
        normal.setStroke(Math.max(1, dp(1)), borderCol);

        GradientDrawable focused = new GradientDrawable();
        focused.setColor(focusBg);
        focused.setCornerRadius(dp(radiusDp));
        focused.setStroke(dp(2), COL_ACCENT_FOCUS);

        GradientDrawable pressed = new GradientDrawable();
        pressed.setColor(pressBg);
        pressed.setCornerRadius(dp(radiusDp));
        pressed.setStroke(dp(2), COL_ACCENT_FOCUS);

        GradientDrawable disabled = new GradientDrawable();
        disabled.setColor(Color.parseColor("#181C26"));
        disabled.setCornerRadius(dp(radiusDp));
        disabled.setStroke(Math.max(1, dp(1)), Color.parseColor("#232838"));

        StateListDrawable sld = new StateListDrawable();
        sld.addState(new int[] { -android.R.attr.state_enabled }, disabled);
        sld.addState(new int[] { android.R.attr.state_pressed }, pressed);
        sld.addState(new int[] { android.R.attr.state_focused }, focused);
        sld.addState(new int[] {}, normal);
        return sld;
    }

    private Drawable makeInputDrawable() {
        GradientDrawable normal = new GradientDrawable();
        normal.setColor(COL_INSET);
        normal.setCornerRadius(dp(8));
        normal.setStroke(Math.max(1, dp(1)), COL_BORDER);

        GradientDrawable focused = new GradientDrawable();
        focused.setColor(COL_INSET);
        focused.setCornerRadius(dp(8));
        focused.setStroke(dp(2), COL_ACCENT_FOCUS);

        StateListDrawable sld = new StateListDrawable();
        sld.addState(new int[] { android.R.attr.state_focused }, focused);
        sld.addState(new int[] {}, normal);
        return sld;
    }

    private ProgressBar makeStyledProgressBar() {
        ProgressBar pb = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        pb.setMax(1000);
        pb.setProgress(0);

        GradientDrawable track = new GradientDrawable();
        track.setColor(COL_INSET);
        track.setCornerRadius(dp(5));
        track.setStroke(Math.max(1, dp(1)), COL_BORDER);

        GradientDrawable fill = new GradientDrawable(
                GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] { COL_ACCENT, COL_ACCENT_FOCUS });
        fill.setCornerRadius(dp(5));
        ClipDrawable clip = new ClipDrawable(fill, Gravity.LEFT, ClipDrawable.HORIZONTAL);

        LayerDrawable layer = new LayerDrawable(new Drawable[] { track, clip });
        layer.setId(0, android.R.id.background);
        layer.setId(1, android.R.id.progress);
        pb.setProgressDrawable(layer);
        return pb;
    }

    private Button makePrimaryButton(String text, int heightDp, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, heightDp >= 48 ? 15f : 13.5f);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(makeButtonDrawable(COL_ACCENT, COL_ACCENT_FOCUS, COL_ACCENT_PRESS, COL_ACCENT, 10));
        b.setPadding(dp(20), dp(10), dp(20), dp(10));
        b.setMinimumHeight(dp(heightDp));
        b.setMinHeight(dp(heightDp));
        b.setStateListAnimator(null);
        b.setFocusable(true);
        b.setFocusableInTouchMode(false);
        b.setOnClickListener(listener);
        return b;
    }

    private Button makeSecondaryButton(String text, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(COL_TEXT_PRIMARY);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(makeButtonDrawable(COL_ROW_ALT, Color.parseColor("#2A3145"), COL_INSET, COL_BORDER, 8));
        b.setPadding(dp(14), dp(8), dp(14), dp(8));
        b.setMinimumHeight(dp(40));
        b.setMinHeight(dp(40));
        b.setStateListAnimator(null);
        b.setFocusable(true);
        b.setFocusableInTouchMode(false);
        b.setOnClickListener(listener);
        return b;
    }

    private Button makeDangerButton(String text, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(COL_TEXT_PRIMARY);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(makeButtonDrawable(
                Color.parseColor("#3B1B22"), Color.parseColor("#54222D"), Color.parseColor("#2A1217"), COL_DANGER, 8));
        b.setPadding(dp(14), dp(8), dp(14), dp(8));
        b.setMinimumHeight(dp(40));
        b.setMinHeight(dp(40));
        b.setStateListAnimator(null);
        b.setFocusable(true);
        b.setFocusableInTouchMode(false);
        b.setOnClickListener(listener);
        return b;
    }

    private LinearLayout makeCard(String title, String subtitle) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(makeCardDrawable(COL_CARD, COL_BORDER, 12));
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        card.setLayoutParams(lp);

        if (title != null && !title.isEmpty()) {
            TextView tv = new TextView(this);
            tv.setText(title);
            tv.setTextColor(COL_TEXT_PRIMARY);
            tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
            tv.setTypeface(Typeface.DEFAULT_BOLD);
            card.addView(tv);
        }

        if (subtitle != null && !subtitle.isEmpty()) {
            TextView sub = new TextView(this);
            sub.setText(subtitle);
            sub.setTextColor(COL_TEXT_SECONDARY);
            sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            slp.topMargin = dp(3);
            slp.bottomMargin = dp(12);
            sub.setLayoutParams(slp);
            card.addView(sub);
        }
        return card;
    }

    private TextView makeSectionEyebrow(String text) {
        TextView tv = new TextView(this);
        tv.setText(text.toUpperCase(Locale.ROOT));
        tv.setTextColor(COL_TEXT_MUTED);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setLetterSpacing(0.08f);
        return tv;
    }

    private LinearLayout makeHRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    private void setRightMargin(View v, int rightDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.rightMargin = dp(rightDp);
        v.setLayoutParams(lp);
    }

    private interface OnSegmentSelected {
        void onSelected(int index, String value);
    }

    private SegmentedControl addSegmentedSettingRow(
            LinearLayout parent, String label, String[] labels, String[] values, OnSegmentSelected onSelected) {
        TextView lbl = new TextView(this);
        lbl.setText(label);
        lbl.setTextColor(COL_TEXT_SECONDARY);
        lbl.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        lbl.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        llp.topMargin = dp(6);
        llp.bottomMargin = dp(5);
        lbl.setLayoutParams(llp);
        parent.addView(lbl);

        LinearLayout track = new LinearLayout(this);
        track.setOrientation(LinearLayout.HORIZONTAL);
        track.setBackground(makeCardDrawable(COL_INSET, COL_BORDER, 9));
        track.setPadding(dp(3), dp(3), dp(3), dp(3));
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        tlp.bottomMargin = dp(6);
        track.setLayoutParams(tlp);

        Button[] btns = new Button[labels.length];
        SegmentedControl sc = new SegmentedControl(btns, values);
        for (int i = 0; i < labels.length; ++i) {
            final int idx = i;
            Button b = new Button(this);
            b.setText(labels[i]);
            b.setAllCaps(false);
            b.setSingleLine(true);
            b.setEllipsize(TextUtils.TruncateAt.END);
            b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            b.setTypeface(Typeface.DEFAULT_BOLD);
            b.setPadding(dp(6), dp(7), dp(6), dp(7));
            b.setMinimumHeight(dp(34));
            b.setMinHeight(dp(34));
            b.setStateListAnimator(null);
            b.setFocusable(true);
            b.setFocusableInTouchMode(false);
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            if (i > 0) blp.leftMargin = dp(3);
            b.setLayoutParams(blp);
            b.setOnClickListener(v -> {
                setSegmentedIndex(sc, idx);
                if (onSelected != null) onSelected.onSelected(idx, values[idx]);
            });
            btns[i] = b;
            track.addView(b);
        }
        parent.addView(track);
        setSegmentedIndex(sc, 0);
        return sc;
    }

    private void setSegmentedIndex(SegmentedControl sc, int index) {
        if (sc == null || sc.buttons.length == 0) return;
        int clamped = Math.max(0, Math.min(index, sc.buttons.length - 1));
        sc.selectedIndex = clamped;
        for (int i = 0; i < sc.buttons.length; ++i) {
            boolean active = (i == clamped);
            Button b = sc.buttons[i];
            if (active) {
                b.setBackground(makeButtonDrawable(
                        COL_ACCENT_SOFT, Color.parseColor("#42E8195E"), COL_ACCENT_SOFT, COL_ACCENT, 7));
                b.setTextColor(Color.WHITE);
            } else {
                b.setBackground(makeButtonDrawable(
                        Color.TRANSPARENT, COL_ROW_ALT, COL_INSET, Color.TRANSPARENT, 7));
                b.setTextColor(COL_TEXT_SECONDARY);
            }
        }
    }

    // ---- Main Responsive Shell ----------------------------------------------

    private void buildUi() {
        mNavTabViews.clear();
        mNavTabIndicators.clear();
        mNavTabLabels.clear();

        mRootContainer = new LinearLayout(this);
        mRootContainer.setBackgroundColor(COL_CANVAS);
        mRootContainer.setOrientation(mIsLandscapeLayout ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);

        // Apply display cutout & system bar safe insets cleanly
        mRootContainer.setOnApplyWindowInsetsListener((v, insets) -> {
            int left = 0, top = 0, right = 0, bottom = 0;
            if (insets != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    android.graphics.Insets sys = insets.getInsets(
                            WindowInsets.Type.displayCutout() | WindowInsets.Type.systemBars());
                    left = sys.left;
                    top = sys.top;
                    right = sys.right;
                    bottom = sys.bottom;
                } else {
                    DisplayCutout cutout = insets.getDisplayCutout();
                    if (cutout != null) {
                        left = cutout.getSafeInsetLeft();
                        top = cutout.getSafeInsetTop();
                        right = cutout.getSafeInsetRight();
                        bottom = cutout.getSafeInsetBottom();
                    }
                }
            }
            v.setPadding(left, top, right, bottom);
            return insets;
        });

        if (mIsLandscapeLayout) {
            mRootContainer.addView(buildLandscapeRail());
            View divider = new View(this);
            divider.setBackgroundColor(COL_BORDER);
            divider.setLayoutParams(new LinearLayout.LayoutParams(Math.max(1, dp(1)), ViewGroup.LayoutParams.MATCH_PARENT));
            mRootContainer.addView(divider);
        } else {
            mRootContainer.addView(buildPortraitTopChrome());
            View divider = new View(this);
            divider.setBackgroundColor(COL_BORDER);
            divider.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(1))));
            mRootContainer.addView(divider);
        }

        // Right / Main Content Deck
        FrameLayout contentDeck = new FrameLayout(this);
        LinearLayout.LayoutParams dlp = mIsLandscapeLayout
                ? new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        contentDeck.setLayoutParams(dlp);
        contentDeck.setBackgroundColor(COL_CANVAS);

        mTabContainers[TAB_PLAY] = wrapInScrollPane(buildPlayTab());
        mTabContainers[TAB_SETUP] = wrapInScrollPane(buildSetupTab());
        mTabContainers[TAB_CONTENT] = wrapInScrollPane(buildContentTab());
        mTabContainers[TAB_SAVES] = wrapInScrollPane(buildSavesTab());
        mTabContainers[TAB_WIFI] = wrapInScrollPane(buildWifiTab());
        mTabContainers[TAB_SETTINGS] = wrapInScrollPane(buildSettingsTab());

        for (FrameLayout pane : mTabContainers) {
            contentDeck.addView(pane, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }

        mRootContainer.addView(contentDeck);
        setContentView(mRootContainer);
        mRootContainer.requestApplyInsets();

        syncSettingsControls();
        selectTab(mActiveTab);
    }

    private FrameLayout wrapInScrollPane(View inner) {
        FrameLayout wrapper = new FrameLayout(this);
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.setVerticalScrollBarEnabled(false);
        sv.setPadding(dp(18), dp(16), dp(18), dp(18));
        sv.setClipToPadding(false);
        sv.addView(inner, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        wrapper.addView(sv, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        return wrapper;
    }

    private LinearLayout buildLandscapeRail() {
        LinearLayout rail = new LinearLayout(this);
        rail.setOrientation(LinearLayout.VERTICAL);
        rail.setBackgroundColor(COL_SURFACE);
        rail.setPadding(dp(14), dp(16), dp(14), dp(14));
        rail.setLayoutParams(new LinearLayout.LayoutParams(dp(200), ViewGroup.LayoutParams.MATCH_PARENT));

        // Brand Header with vector ShiftLogoView
        LinearLayout brand = makeHRow();
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.bottomMargin = dp(16);
        brand.setLayoutParams(blp);

        ShiftLogoView logo = new ShiftLogoView(this);
        LinearLayout.LayoutParams logoLp = new LinearLayout.LayoutParams(dp(36), dp(36));
        logoLp.rightMargin = dp(10);
        logo.setLayoutParams(logoLp);
        brand.addView(logo);

        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        TextView appTitle = new TextView(this);
        appTitle.setText("SHIFT ENGINE");
        appTitle.setTextColor(COL_TEXT_PRIMARY);
        appTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        appTitle.setTypeface(Typeface.DEFAULT_BOLD);
        appTitle.setLetterSpacing(0.06f);
        titles.addView(appTitle);

        TextView appVer = new TextView(this);
        appVer.setText("v0.3  •  Android");
        appVer.setTextColor(COL_TEXT_MUTED);
        appVer.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        titles.addView(appVer);
        brand.addView(titles);
        rail.addView(brand);

        // Nav Tabs (scrollable if screen height is short)
        ScrollView navScroll = new ScrollView(this);
        navScroll.setVerticalScrollBarEnabled(false);
        LinearLayout.LayoutParams nslp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        navScroll.setLayoutParams(nslp);

        LinearLayout navList = new LinearLayout(this);
        navList.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < TAB_NAMES.length; ++i) {
            final int tabIdx = i;
            LinearLayout item = makeHRow();
            item.setFocusable(true);
            item.setFocusableInTouchMode(false);
            item.setClickable(true);
            item.setPadding(dp(8), dp(10), dp(10), dp(10));
            LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            ilp.bottomMargin = dp(4);
            item.setLayoutParams(ilp);

            View bar = new View(this);
            LinearLayout.LayoutParams barLp = new LinearLayout.LayoutParams(dp(3), dp(18));
            barLp.rightMargin = dp(10);
            bar.setLayoutParams(barLp);
            item.addView(bar);

            TextView tv = new TextView(this);
            tv.setText(TAB_NAMES[i]);
            tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
            tv.setTypeface(Typeface.DEFAULT_BOLD);
            item.addView(tv);

            item.setOnClickListener(v -> selectTab(tabIdx));
            mNavTabViews.add(item);
            mNavTabIndicators.add(bar);
            mNavTabLabels.add(tv);
            navList.addView(item);
        }
        navScroll.addView(navList);
        rail.addView(navScroll);

        // Gamepad hint
        TextView hint = new TextView(this);
        hint.setText("L1 / R1 Tabs  •  Start Play");
        hint.setTextColor(COL_TEXT_MUTED);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f);
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hlp.topMargin = dp(8);
        hlp.bottomMargin = dp(8);
        hint.setLayoutParams(hlp);
        rail.addView(hint);

        // Bottom Status Pill
        LinearLayout pill = new LinearLayout(this);
        pill.setOrientation(LinearLayout.VERTICAL);
        pill.setBackground(makeCardDrawable(COL_INSET, COL_BORDER, 10));
        pill.setPadding(dp(10), dp(8), dp(10), dp(8));

        LinearLayout topLine = makeHRow();
        mStatusPillDot = new View(this);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(dp(8), dp(8));
        dlp.rightMargin = dp(7);
        mStatusPillDot.setLayoutParams(dlp);
        mStatusPillDot.setBackground(makeCircleDrawable(COL_WARNING));
        topLine.addView(mStatusPillDot);

        mStatusPillLabel = new TextView(this);
        mStatusPillLabel.setText("Checking...");
        mStatusPillLabel.setTextColor(COL_TEXT_PRIMARY);
        mStatusPillLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        mStatusPillLabel.setTypeface(Typeface.DEFAULT_BOLD);
        topLine.addView(mStatusPillLabel);
        pill.addView(topLine);

        mStatusStorageLabel = new TextView(this);
        mStatusStorageLabel.setText("Scanning storage...");
        mStatusStorageLabel.setTextColor(COL_TEXT_MUTED);
        mStatusStorageLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        LinearLayout.LayoutParams stlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        stlp.topMargin = dp(2);
        mStatusStorageLabel.setLayoutParams(stlp);
        pill.addView(mStatusStorageLabel);

        rail.addView(pill);
        return rail;
    }

    private LinearLayout buildPortraitTopChrome() {
        LinearLayout chrome = new LinearLayout(this);
        chrome.setOrientation(LinearLayout.VERTICAL);
        chrome.setBackgroundColor(COL_SURFACE);
        chrome.setPadding(dp(14), dp(12), dp(14), dp(10));
        chrome.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // Top Brand Bar + Status Pill
        LinearLayout topBar = makeHRow();
        LinearLayout.LayoutParams tblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        tblp.bottomMargin = dp(10);
        topBar.setLayoutParams(tblp);

        ShiftLogoView logo = new ShiftLogoView(this);
        LinearLayout.LayoutParams logoLp = new LinearLayout.LayoutParams(dp(30), dp(30));
        logoLp.rightMargin = dp(8);
        logo.setLayoutParams(logoLp);
        topBar.addView(logo);

        LinearLayout titleCol = new LinearLayout(this);
        titleCol.setOrientation(LinearLayout.VERTICAL);
        titleCol.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView appTitle = new TextView(this);
        appTitle.setText("SHIFT ENGINE");
        appTitle.setTextColor(COL_TEXT_PRIMARY);
        appTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        appTitle.setTypeface(Typeface.DEFAULT_BOLD);
        appTitle.setLetterSpacing(0.05f);
        titleCol.addView(appTitle);

        mStatusStorageLabel = new TextView(this);
        mStatusStorageLabel.setText("v0.3  •  Scanning storage...");
        mStatusStorageLabel.setTextColor(COL_TEXT_MUTED);
        mStatusStorageLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        titleCol.addView(mStatusStorageLabel);
        topBar.addView(titleCol);

        LinearLayout pill = makeHRow();
        pill.setBackground(makePillDrawable(COL_INSET, COL_BORDER, 14));
        pill.setPadding(dp(10), dp(5), dp(10), dp(5));

        mStatusPillDot = new View(this);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(dp(7), dp(7));
        dlp.rightMargin = dp(6);
        mStatusPillDot.setLayoutParams(dlp);
        mStatusPillDot.setBackground(makeCircleDrawable(COL_WARNING));
        pill.addView(mStatusPillDot);

        mStatusPillLabel = new TextView(this);
        mStatusPillLabel.setText("Checking");
        mStatusPillLabel.setTextColor(COL_TEXT_PRIMARY);
        mStatusPillLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        mStatusPillLabel.setTypeface(Typeface.DEFAULT_BOLD);
        pill.addView(mStatusPillLabel);
        topBar.addView(pill);

        chrome.addView(topBar);

        // Scrollable Segmented Tab Bar
        HorizontalScrollView hsv = new HorizontalScrollView(this);
        hsv.setHorizontalScrollBarEnabled(false);
        LinearLayout tabRow = makeHRow();
        tabRow.setBackground(makeCardDrawable(COL_INSET, COL_BORDER, 10));
        tabRow.setPadding(dp(4), dp(4), dp(4), dp(4));

        for (int i = 0; i < TAB_NAMES.length; ++i) {
            final int tabIdx = i;
            LinearLayout tab = new LinearLayout(this);
            tab.setOrientation(LinearLayout.VERTICAL);
            tab.setGravity(Gravity.CENTER);
            tab.setFocusable(true);
            tab.setFocusableInTouchMode(false);
            tab.setClickable(true);
            tab.setPadding(dp(12), dp(7), dp(12), dp(6));
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            if (i > 0) tlp.leftMargin = dp(4);
            tab.setLayoutParams(tlp);

            TextView tv = new TextView(this);
            tv.setText(TAB_NAMES[i]);
            tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
            tv.setTypeface(Typeface.DEFAULT_BOLD);
            tab.addView(tv);

            View underline = new View(this);
            LinearLayout.LayoutParams ulp = new LinearLayout.LayoutParams(dp(22), dp(2));
            ulp.topMargin = dp(3);
            underline.setLayoutParams(ulp);
            tab.addView(underline);

            tab.setOnClickListener(v -> selectTab(tabIdx));
            mNavTabViews.add(tab);
            mNavTabIndicators.add(underline);
            mNavTabLabels.add(tv);
            tabRow.addView(tab);
        }
        hsv.addView(tabRow);
        chrome.addView(hsv);
        return chrome;
    }

    private void selectTab(int idx) {
        mActiveTab = Math.max(0, Math.min(idx, TAB_NAMES.length - 1));
        for (int i = 0; i < mTabContainers.length; ++i) {
            if (mTabContainers[i] != null) {
                mTabContainers[i].setVisibility(i == mActiveTab ? View.VISIBLE : View.GONE);
            }
        }
        for (int i = 0; i < mNavTabViews.size(); ++i) {
            boolean active = (i == mActiveTab);
            View item = mNavTabViews.get(i);
            View ind = mNavTabIndicators.get(i);
            TextView tv = mNavTabLabels.get(i);

            if (active) {
                item.setBackground(makeButtonDrawable(
                        COL_ACCENT_SOFT, Color.parseColor("#3BE8195E"), COL_ACCENT_SOFT, COL_ACCENT, 8));
                ind.setBackground(makePillDrawable(COL_ACCENT, 0, 2));
                ind.setVisibility(View.VISIBLE);
                tv.setTextColor(Color.WHITE);
            } else {
                item.setBackground(makeButtonDrawable(
                        Color.TRANSPARENT, COL_ROW_ALT, COL_INSET, Color.TRANSPARENT, 8));
                ind.setBackgroundColor(Color.TRANSPARENT);
                tv.setTextColor(COL_TEXT_SECONDARY);
            }
        }
    }

    // ---- Tab 1: Play --------------------------------------------------------

    private View buildPlayTab() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        // Hero Card
        LinearLayout hero = new LinearLayout(this);
        hero.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable heroBg = new GradientDrawable(
                GradientDrawable.Orientation.TL_BR,
                new int[] { Color.parseColor("#241928"), COL_CARD, COL_SURFACE });
        heroBg.setCornerRadius(dp(14));
        heroBg.setStroke(Math.max(1, dp(1)), COL_BORDER);
        hero.setBackground(heroBg);
        hero.setPadding(dp(20), dp(18), dp(20), dp(18));
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hlp.bottomMargin = dp(12);
        hero.setLayoutParams(hlp);

        TextView eyebrow = makeSectionEyebrow("SHIFT DRIVE  •  COLORADO");
        eyebrow.setTextColor(COL_ACCENT_FOCUS);
        hero.addView(eyebrow);

        TextView gameTitle = new TextView(this);
        gameTitle.setText("Forza Horizon");
        gameTitle.setTextColor(COL_TEXT_PRIMARY);
        gameTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26f);
        gameTitle.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams gtlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        gtlp.topMargin = dp(2);
        gtlp.bottomMargin = dp(8);
        gameTitle.setLayoutParams(gtlp);
        hero.addView(gameTitle);

        LinearLayout statusRow = makeHRow();
        mHeroStatusDot = new View(this);
        LinearLayout.LayoutParams hdlp = new LinearLayout.LayoutParams(dp(9), dp(9));
        hdlp.rightMargin = dp(8);
        mHeroStatusDot.setLayoutParams(hdlp);
        mHeroStatusDot.setBackground(makeCircleDrawable(COL_WARNING));
        statusRow.addView(mHeroStatusDot);

        mHeroStatusText = new TextView(this);
        mHeroStatusText.setText("Inspecting on-device library...");
        mHeroStatusText.setTextColor(COL_TEXT_PRIMARY);
        mHeroStatusText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        mHeroStatusText.setTypeface(Typeface.DEFAULT_BOLD);
        statusRow.addView(mHeroStatusText);
        hero.addView(statusRow);

        mHeroProfileText = new TextView(this);
        mHeroProfileText.setText("Profile: Player  •  No career save yet");
        mHeroProfileText.setTextColor(COL_TEXT_SECONDARY);
        mHeroProfileText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        LinearLayout.LayoutParams hplp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hplp.topMargin = dp(4);
        hplp.bottomMargin = dp(16);
        mHeroProfileText.setLayoutParams(hplp);
        hero.addView(mHeroProfileText);

        LinearLayout ctaRow = makeHRow();
        mBtnHeroPlay = makePrimaryButton("PLAY", 48, v -> launchDriveWithAnimation());
        mBtnHeroPlay.setEnabled(false);
        LinearLayout.LayoutParams playLp = new LinearLayout.LayoutParams(dp(180), ViewGroup.LayoutParams.WRAP_CONTENT);
        playLp.rightMargin = dp(10);
        mBtnHeroPlay.setLayoutParams(playLp);
        ctaRow.addView(mBtnHeroPlay);

        mBtnHeroGoSetup = makeSecondaryButton("Setup Game Disc", v -> selectTab(TAB_SETUP));
        ctaRow.addView(mBtnHeroGoSetup);
        hero.addView(ctaRow);

        col.addView(hero);

        // Quick Graphics & Controls Card
        LinearLayout quickCard = makeCard(
                "Quick Graphics & Controls",
                "Changes save immediately to settings.txt and apply on launch.");

        mPlayPresetSeg = addSegmentedSettingRow(
                quickCard, "Graphics Preset",
                LauncherSettingsStore.PRESET_LABELS, LauncherSettingsStore.PRESET_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_PRESET, val));
        mPlayTouchSeg = addSegmentedSettingRow(
                quickCard, "Touch Controls",
                LauncherSettingsStore.TOUCH_LABELS, LauncherSettingsStore.TOUCH_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_TOUCH_CONTROLS, val));
        mPlaySteerSeg = addSegmentedSettingRow(
                quickCard, "Touch Steering",
                LauncherSettingsStore.STEER_LABELS, LauncherSettingsStore.STEER_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_TOUCH_STEER, val));
        mPlayFpsSeg = addSegmentedSettingRow(
                quickCard, "FPS Cap",
                LauncherSettingsStore.FPS_LABELS, LauncherSettingsStore.FPS_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_FPS_CAP, val));
        col.addView(quickCard);

        // Compact Library Summary Card
        LinearLayout sumCard = makeCard("Library Summary", null);
        LinearLayout gridRow1 = makeHRow();
        LinearLayout.LayoutParams gr1 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        gr1.topMargin = dp(10);
        gr1.bottomMargin = dp(8);
        gridRow1.setLayoutParams(gr1);

        mSummaryV4Val = addMetricTile(gridRow1, "TITLE UPDATE V4", "Checking...", true);
        mSummaryDlcVal = addMetricTile(gridRow1, "DLC PACKS", "0 installed", false);
        sumCard.addView(gridRow1);

        LinearLayout gridRow2 = makeHRow();
        mSummaryModsVal = addMetricTile(gridRow2, "MODS ENABLED", "0 / 0", true);
        mSummaryDriverVal = addMetricTile(gridRow2, "VULKAN DRIVER", "System", false);
        sumCard.addView(gridRow2);

        col.addView(sumCard);
        return col;
    }

    private TextView addMetricTile(LinearLayout row, String label, String initialValue, boolean rightGap) {
        LinearLayout tile = new LinearLayout(this);
        tile.setOrientation(LinearLayout.VERTICAL);
        tile.setBackground(makeCardDrawable(COL_INSET, COL_BORDER, 9));
        tile.setPadding(dp(12), dp(10), dp(12), dp(10));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        if (rightGap) lp.rightMargin = dp(8);
        tile.setLayoutParams(lp);

        tile.addView(makeSectionEyebrow(label));

        TextView val = new TextView(this);
        val.setText(initialValue);
        val.setTextColor(COL_TEXT_PRIMARY);
        val.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        val.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams vlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        vlp.topMargin = dp(3);
        val.setLayoutParams(vlp);
        tile.addView(val);
        row.addView(tile);
        return val;
    }

    // ---- Tab 2: Setup -------------------------------------------------------

    private View buildSetupTab() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        // Disc Card
        LinearLayout discCard = makeCard(
                "Game Disc (.ISO or Extracted Folder)",
                "Select your Forza Horizon Xbox 360 .iso (streams via Android SAF) or an extracted media/ folder.");

        mDiscStatusText = new TextView(this);
        mDiscStatusText.setText("Checking game disc...");
        mDiscStatusText.setTextColor(COL_WARNING);
        mDiscStatusText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        mDiscStatusText.setTypeface(Typeface.DEFAULT_BOLD);
        discCard.addView(mDiscStatusText);

        mDiscHintText = new TextView(this);
        mDiscHintText.setText("Requires ~7 GB free storage. Cars stream directly from disc with zero car bake.");
        mDiscHintText.setTextColor(COL_TEXT_SECONDARY);
        mDiscHintText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        LinearLayout.LayoutParams dhlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        dhlp.topMargin = dp(4);
        dhlp.bottomMargin = dp(12);
        mDiscHintText.setLayoutParams(dhlp);
        discCard.addView(mDiscHintText);

        LinearLayout pickRow = makeHRow();
        mBtnPickIso = makeSecondaryButton("Select .ISO", v -> openIsoPicker());
        setRightMargin(mBtnPickIso, 10);
        mBtnPickFolder = makeSecondaryButton("Select Extracted Folder", v -> openFolderPicker());
        pickRow.addView(mBtnPickIso);
        pickRow.addView(mBtnPickFolder);
        discCard.addView(pickRow);
        col.addView(discCard);

        // Pipeline & Progress Card
        LinearLayout pipeCard = makeCard(
                "World Index & Verification",
                "Extracts queued .ISO (if selected), builds the compact Colorado world index (~2 s), and writes play.txt.");

        mSetupStageLabelView = new TextView(this);
        mSetupStageLabelView.setText(mSetupStageLabel);
        mSetupStageLabelView.setTextColor(COL_TEXT_PRIMARY);
        mSetupStageLabelView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        mSetupStageLabelView.setTypeface(Typeface.DEFAULT_BOLD);
        pipeCard.addView(mSetupStageLabelView);

        mSetupStageDetailView = new TextView(this);
        mSetupStageDetailView.setText(mSetupStageDetail);
        mSetupStageDetailView.setTextColor(COL_TEXT_SECONDARY);
        mSetupStageDetailView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        pipeCard.addView(mSetupStageDetailView);

        mSetupProgressBar = makeStyledProgressBar();
        LinearLayout.LayoutParams pblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(10));
        pblp.topMargin = dp(8);
        pblp.bottomMargin = dp(14);
        mSetupProgressBar.setLayoutParams(pblp);
        pipeCard.addView(mSetupProgressBar);

        LinearLayout actionRow = makeHRow();
        mBtnRunSetup = makePrimaryButton("Run Setup", 42, v -> runSetupPipeline());
        setRightMargin(mBtnRunSetup, 10);
        mBtnCancelSetup = makeDangerButton("Cancel", v -> cancelActiveWork());
        mBtnCancelSetup.setEnabled(false);
        setRightMargin(mBtnCancelSetup, 10);
        mBtnToggleSetupLog = makeSecondaryButton(mSetupLogVisible ? "Hide Log" : "Show Log", v -> toggleSetupLogVisibility());

        actionRow.addView(mBtnRunSetup);
        actionRow.addView(mBtnCancelSetup);
        actionRow.addView(mBtnToggleSetupLog);
        pipeCard.addView(actionRow);

        // Collapsible Monospace Setup Log Viewer
        mSetupLogScroll = new ScrollView(this);
        mSetupLogScroll.setBackground(makeCardDrawable(COL_INSET, COL_BORDER, 8));
        mSetupLogScroll.setPadding(dp(10), dp(8), dp(10), dp(8));
        mSetupLogScroll.setVisibility(mSetupLogVisible ? View.VISIBLE : View.GONE);
        LinearLayout.LayoutParams lslp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(160));
        lslp.topMargin = dp(12);
        mSetupLogScroll.setLayoutParams(lslp);

        mSetupLogText = new TextView(this);
        mSetupLogText.setText(mSetupLogBuf.toString());
        mSetupLogText.setTextColor(COL_TEXT_SECONDARY);
        mSetupLogText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        mSetupLogText.setTypeface(Typeface.MONOSPACE);
        mSetupLogScroll.addView(mSetupLogText);
        pipeCard.addView(mSetupLogScroll);

        col.addView(pipeCard);
        return col;
    }

    // ---- Tab 3: Content & Mods ----------------------------------------------

    private View buildContentTab() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        // Updates & DLC Card
        LinearLayout dlcCard = makeCard(
                "Updates & DLC (Xbox 360 STFS)",
                "Import Title Update v4, Horizon Rally, 1000 Club, and DLC car packs directly on-device (multi-select supported).");

        mBtnAddStfs = makePrimaryButton("Import STFS", 40, v -> openStfsPicker());
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.bottomMargin = dp(10);
        mBtnAddStfs.setLayoutParams(blp);
        dlcCard.addView(mBtnAddStfs);

        mPackagesList = new LinearLayout(this);
        mPackagesList.setOrientation(LinearLayout.VERTICAL);
        dlcCard.addView(mPackagesList);
        col.addView(dlcCard);

        // Mods Card
        LinearLayout modsCard = makeCard(
                "Mods (.ZIP Archives)",
                "Install mods into state/mods/, toggle enable/disable, reorder load priority, or remove.");

        mBtnAddMod = makeSecondaryButton("Install Mod .ZIP", v -> openModZipPicker());
        LinearLayout.LayoutParams mblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        mblp.bottomMargin = dp(10);
        mBtnAddMod.setLayoutParams(mblp);
        modsCard.addView(mBtnAddMod);

        mModsList = new LinearLayout(this);
        mModsList.setOrientation(LinearLayout.VERTICAL);
        modsCard.addView(mModsList);
        col.addView(modsCard);

        return col;
    }

    // ---- Tab 4: Saves -------------------------------------------------------

    private View buildSavesTab() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        LinearLayout activeCard = makeCard(
                "Career Save & Profile",
                "Local Android career save (drive/save.txt + profiles.txt) with rolling snapshot backups and plaintext export .ZIP import.");

        mSaveProfileTitle = new TextView(this);
        mSaveProfileTitle.setText("Active Profile: Player");
        mSaveProfileTitle.setTextColor(COL_TEXT_PRIMARY);
        mSaveProfileTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        mSaveProfileTitle.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams ptlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        ptlp.bottomMargin = dp(10);
        mSaveProfileTitle.setLayoutParams(ptlp);
        activeCard.addView(mSaveProfileTitle);

        LinearLayout row1 = makeHRow();
        LinearLayout.LayoutParams r1lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        r1lp.bottomMargin = dp(8);
        row1.setLayoutParams(r1lp);
        mSaveMetaCredits = addMetricTile(row1, "CREDITS", "—", true);
        mSaveMetaGarage = addMetricTile(row1, "GARAGE CARS", "—", false);
        activeCard.addView(row1);

        LinearLayout row2 = makeHRow();
        LinearLayout.LayoutParams r2lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        r2lp.bottomMargin = dp(12);
        row2.setLayoutParams(r2lp);
        mSaveMetaSigns = addMetricTile(row2, "DISCOUNT SIGNS", "—", true);
        mSaveMetaCar = addMetricTile(row2, "CURRENT CAR", "—", false);
        activeCard.addView(row2);

        mSaveStatusBanner = new TextView(this);
        mSaveStatusBanner.setTextColor(COL_TEXT_SECONDARY);
        mSaveStatusBanner.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        LinearLayout.LayoutParams sblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sblp.bottomMargin = dp(12);
        mSaveStatusBanner.setLayoutParams(sblp);
        mSaveStatusBanner.setVisibility(View.GONE);
        activeCard.addView(mSaveStatusBanner);

        LinearLayout btnRow = makeHRow();
        mBtnBackupSave = makeSecondaryButton("Backup Save", v -> backupSaveAsync());
        setRightMargin(mBtnBackupSave, 10);
        mBtnImportSaveZip = makePrimaryButton("Import Save Export (.zip)", 40, v -> openSaveZipPicker());
        btnRow.addView(mBtnBackupSave);
        btnRow.addView(mBtnImportSaveZip);
        activeCard.addView(btnRow);

        col.addView(activeCard);

        LinearLayout backupsCard = makeCard(
                "Rolling Backups",
                "Automatic and manual snapshots stored under backups/saves/.");
        mBackupsList = new LinearLayout(this);
        mBackupsList.setOrientation(LinearLayout.VERTICAL);
        backupsCard.addView(mBackupsList);
        col.addView(backupsCard);

        return col;
    }

    // ---- Tab 5: Wi-Fi Sync --------------------------------------------------

    private View buildWifiTab() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        LinearLayout syncCard = makeCard(
                "Wi-Fi Sync from PC Launcher",
                "Pair with the desktop Shift Engine Launcher on the same LAN to transfer play/, DLC, and mods over resumable HTTP.");

        mBtnWifiDiscover = makeSecondaryButton("Discover PC on LAN", v -> discoverWifiHostAsync());
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        dlp.bottomMargin = dp(12);
        mBtnWifiDiscover.setLayoutParams(dlp);
        syncCard.addView(mBtnWifiDiscover);

        // Host IP + Port + Pairing Code inputs
        LinearLayout fieldsRow = makeHRow();
        LinearLayout.LayoutParams frlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        frlp.bottomMargin = dp(12);
        fieldsRow.setLayoutParams(frlp);

        mEditWifiHost = addLabeledInput(fieldsRow, "HOST IP", "192.168.1.x", mWifiHostVal,
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI, 2.2f, true, 64);
        mEditWifiPort = addLabeledInput(fieldsRow, "PORT", "47616", mWifiPortVal,
                InputType.TYPE_CLASS_NUMBER, 1.0f, true, 5);
        mEditWifiCode = addLabeledInput(fieldsRow, "6-DIGIT CODE", "000000", mWifiCodeVal,
                InputType.TYPE_CLASS_NUMBER, 1.3f, false, 6);
        syncCard.addView(fieldsRow);

        mWifiStatusLabelView = new TextView(this);
        mWifiStatusLabelView.setText(mWifiStatusLabel);
        mWifiStatusLabelView.setTextColor(COL_TEXT_PRIMARY);
        mWifiStatusLabelView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        mWifiStatusLabelView.setTypeface(Typeface.DEFAULT_BOLD);
        syncCard.addView(mWifiStatusLabelView);

        mWifiStatusDetailView = new TextView(this);
        mWifiStatusDetailView.setText(mWifiStatusDetail);
        mWifiStatusDetailView.setTextColor(COL_TEXT_SECONDARY);
        mWifiStatusDetailView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        syncCard.addView(mWifiStatusDetailView);

        mWifiProgressBar = makeStyledProgressBar();
        LinearLayout.LayoutParams pblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(10));
        pblp.topMargin = dp(8);
        pblp.bottomMargin = dp(12);
        mWifiProgressBar.setLayoutParams(pblp);
        syncCard.addView(mWifiProgressBar);

        LinearLayout btnRow = makeHRow();
        mBtnWifiSync = makePrimaryButton("Sync Now", 42, v -> runWifiSyncAsync());
        setRightMargin(mBtnWifiSync, 10);
        mBtnWifiCancel = makeDangerButton("Cancel", v -> cancelActiveWork());
        mBtnWifiCancel.setEnabled(false);
        btnRow.addView(mBtnWifiSync);
        btnRow.addView(mBtnWifiCancel);
        syncCard.addView(btnRow);

        // Wi-Fi Activity Log
        mWifiLogScroll = new ScrollView(this);
        mWifiLogScroll.setBackground(makeCardDrawable(COL_INSET, COL_BORDER, 8));
        mWifiLogScroll.setPadding(dp(10), dp(8), dp(10), dp(8));
        LinearLayout.LayoutParams lslp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(145));
        lslp.topMargin = dp(12);
        mWifiLogScroll.setLayoutParams(lslp);

        mWifiLogText = new TextView(this);
        mWifiLogText.setText(mWifiLogBuf.length() > 0
                ? mWifiLogBuf.toString()
                : "Ready. Tap 'Discover PC on LAN' or enter the host IP and 6-digit pairing code.\n");
        mWifiLogText.setTextColor(COL_TEXT_SECONDARY);
        mWifiLogText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        mWifiLogText.setTypeface(Typeface.MONOSPACE);
        mWifiLogScroll.addView(mWifiLogText);
        syncCard.addView(mWifiLogScroll);

        col.addView(syncCard);
        return col;
    }

    private EditText addLabeledInput(
            LinearLayout row, String label, String hint, String initialText,
            int inputType, float weight, boolean rightGap, int maxLen) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, weight);
        if (rightGap) blp.rightMargin = dp(8);
        box.setLayoutParams(blp);

        TextView lbl = makeSectionEyebrow(label);
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        llp.bottomMargin = dp(4);
        lbl.setLayoutParams(llp);
        box.addView(lbl);

        EditText et = new EditText(this);
        et.setHint(hint);
        et.setText(initialText);
        et.setHintTextColor(COL_TEXT_MUTED);
        et.setTextColor(COL_TEXT_PRIMARY);
        et.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        et.setSingleLine(true);
        et.setInputType(inputType);
        et.setFilters(new InputFilter[] { new InputFilter.LengthFilter(maxLen) });
        et.setBackground(makeInputDrawable());
        et.setPadding(dp(10), dp(9), dp(10), dp(9));
        et.setMinimumHeight(dp(40));
        box.addView(et);
        row.addView(box);
        return et;
    }

    private void captureWifiInputs() {
        if (mEditWifiHost != null) mWifiHostVal = mEditWifiHost.getText().toString().trim();
        if (mEditWifiPort != null) mWifiPortVal = mEditWifiPort.getText().toString().trim();
        if (mEditWifiCode != null) mWifiCodeVal = mEditWifiCode.getText().toString().trim();
    }

    // ---- Tab 6: Settings & GPU Driver ---------------------------------------

    private View buildSettingsTab() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        LinearLayout gfxCard = makeCard(
                "Graphics & Display Settings",
                "Synced directly with settings.txt (used immediately when launching Shift Drive).");

        mSetPresetSeg = addSegmentedSettingRow(
                gfxCard, "Graphics Preset",
                LauncherSettingsStore.PRESET_LABELS, LauncherSettingsStore.PRESET_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_PRESET, val));
        mSetVsyncSeg = addSegmentedSettingRow(
                gfxCard, "VSync Present Mode",
                LauncherSettingsStore.VSYNC_LABELS, LauncherSettingsStore.VSYNC_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_VSYNC, val));
        mSetFpsSeg = addSegmentedSettingRow(
                gfxCard, "FPS Cap",
                LauncherSettingsStore.FPS_LABELS, LauncherSettingsStore.FPS_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_FPS_CAP, val));
        mSetTouchSeg = addSegmentedSettingRow(
                gfxCard, "Touch Controls",
                LauncherSettingsStore.TOUCH_LABELS, LauncherSettingsStore.TOUCH_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_TOUCH_CONTROLS, val));
        mSetSteerSeg = addSegmentedSettingRow(
                gfxCard, "Touch Steering",
                LauncherSettingsStore.STEER_LABELS, LauncherSettingsStore.STEER_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_TOUCH_STEER, val));
        mSetDriverPrefSeg = addSegmentedSettingRow(
                gfxCard, "Vulkan Driver Preference",
                LauncherSettingsStore.DRIVER_LABELS, LauncherSettingsStore.DRIVER_VALUES,
                (i, val) -> updateSetting(LauncherSettingsStore.KEY_GPU_DRIVER, val));
        col.addView(gfxCard);

        // Custom Vulkan Driver (Mesa Turnip) Card
        LinearLayout gpuCard = makeCard(
                "Custom Vulkan Driver (Mesa Turnip)",
                "Optional for Qualcomm Adreno GPUs: import a Turnip libvulkan_freedreno.so or release .zip into state/drivers/turnip/.");

        mTurnipStatusText = new TextView(this);
        mTurnipStatusText.setText("Active Vulkan driver: System default");
        mTurnipStatusText.setTextColor(COL_TEXT_SECONDARY);
        mTurnipStatusText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        mTurnipStatusText.setTypeface(Typeface.DEFAULT_BOLD);
        gpuCard.addView(mTurnipStatusText);

        mTurnipPathText = new TextView(this);
        mTurnipPathText.setText("No custom Turnip driver imported.");
        mTurnipPathText.setTextColor(COL_TEXT_MUTED);
        mTurnipPathText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        LinearLayout.LayoutParams tplp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        tplp.topMargin = dp(3);
        tplp.bottomMargin = dp(12);
        mTurnipPathText.setLayoutParams(tplp);
        gpuCard.addView(mTurnipPathText);

        mBtnImportTurnip = makeSecondaryButton("Import Turnip .so / .zip", v -> openTurnipPicker());
        gpuCard.addView(mBtnImportTurnip);
        col.addView(gpuCard);

        return col;
    }

    private void syncSettingsControls() {
        String preset = LauncherSettingsStore.getOrDefault(mSettings, LauncherSettingsStore.KEY_PRESET, "smooth-60");
        String touch = LauncherSettingsStore.getOrDefault(mSettings, LauncherSettingsStore.KEY_TOUCH_CONTROLS, "auto");
        String steer = LauncherSettingsStore.getOrDefault(mSettings, LauncherSettingsStore.KEY_TOUCH_STEER, "buttons");
        String fps = LauncherSettingsStore.getOrDefault(mSettings, LauncherSettingsStore.KEY_FPS_CAP, "60");
        String vsync = LauncherSettingsStore.getOrDefault(mSettings, LauncherSettingsStore.KEY_VSYNC, "fifo");
        String drv = LauncherSettingsStore.getOrDefault(mSettings, LauncherSettingsStore.KEY_GPU_DRIVER, "auto");

        int presetIdx = LauncherSettingsStore.indexOfValue(LauncherSettingsStore.PRESET_VALUES, preset, 0);
        int touchIdx = LauncherSettingsStore.indexOfValue(LauncherSettingsStore.TOUCH_VALUES, touch, 0);
        int steerIdx = LauncherSettingsStore.indexOfValue(LauncherSettingsStore.STEER_VALUES, steer, 0);
        int fpsIdx = LauncherSettingsStore.indexOfValue(LauncherSettingsStore.FPS_VALUES, fps, 0);
        int vsyncIdx = LauncherSettingsStore.indexOfValue(LauncherSettingsStore.VSYNC_VALUES, vsync, 0);
        int drvIdx = LauncherSettingsStore.indexOfValue(LauncherSettingsStore.DRIVER_VALUES, drv, 0);

        setSegmentedIndex(mPlayPresetSeg, presetIdx);
        setSegmentedIndex(mPlayTouchSeg, touchIdx);
        setSegmentedIndex(mPlaySteerSeg, steerIdx);
        setSegmentedIndex(mPlayFpsSeg, fpsIdx);

        setSegmentedIndex(mSetPresetSeg, presetIdx);
        setSegmentedIndex(mSetVsyncSeg, vsyncIdx);
        setSegmentedIndex(mSetFpsSeg, fpsIdx);
        setSegmentedIndex(mSetTouchSeg, touchIdx);
        setSegmentedIndex(mSetSteerSeg, steerIdx);
        setSegmentedIndex(mSetDriverPrefSeg, drvIdx);
    }

    private void applyAllCachedStateToUi() {
        syncSettingsControls();
        updateSetupProgressUi(mSetupProgressFrac, mSetupStageLabel, mSetupStageDetail);
        updateWifiProgressUi(mWifiProgressFrac, mWifiStatusLabel, mWifiStatusDetail);
        if (mLastLibraryJson != null) applyLibraryStatus(mLastLibraryJson);
        if (mLastSavesJson != null) applySavesStatus(mLastSavesJson);
        refreshButtonsEnabledState();
    }

    // ---- Logging & Progress Helpers -----------------------------------------

    private void toggleSetupLogVisibility() {
        mSetupLogVisible = !mSetupLogVisible;
        if (mSetupLogScroll != null) {
            mSetupLogScroll.setVisibility(mSetupLogVisible ? View.VISIBLE : View.GONE);
            if (mSetupLogVisible) {
                mSetupLogScroll.post(() -> mSetupLogScroll.fullScroll(View.FOCUS_DOWN));
            }
        }
        if (mBtnToggleSetupLog != null) {
            mBtnToggleSetupLog.setText(mSetupLogVisible ? "Hide Log" : "Show Log");
        }
    }

    private void appendSetupLog(String line) {
        if (line == null) return;
        runOnUiThread(() -> {
            if (mSetupLogBuf.length() > 32768) {
                mSetupLogBuf.delete(0, mSetupLogBuf.length() - 16384);
            }
            mSetupLogBuf.append(line).append('\n');
            if (mSetupLogText != null) {
                mSetupLogText.setText(mSetupLogBuf.toString());
                if (mSetupLogVisible && mSetupLogScroll != null) {
                    mSetupLogScroll.post(() -> mSetupLogScroll.fullScroll(View.FOCUS_DOWN));
                }
            }
        });
    }

    private void appendWifiLog(String line) {
        if (line == null) return;
        runOnUiThread(() -> {
            if (mWifiLogBuf.length() > 32768) {
                mWifiLogBuf.delete(0, mWifiLogBuf.length() - 16384);
            }
            mWifiLogBuf.append(line).append('\n');
            if (mWifiLogText != null) {
                mWifiLogText.setText(mWifiLogBuf.toString());
                if (mWifiLogScroll != null) {
                    mWifiLogScroll.post(() -> mWifiLogScroll.fullScroll(View.FOCUS_DOWN));
                }
            }
        });
    }

    private void updateSetupProgressUi(float fraction, String label, String detail) {
        mSetupProgressFrac = Math.max(0f, Math.min(1f, fraction));
        if (label != null) mSetupStageLabel = label;
        if (detail != null) mSetupStageDetail = detail;
        if (mSetupProgressBar != null) {
            mSetupProgressBar.setProgress(Math.round(mSetupProgressFrac * 1000f));
        }
        if (mSetupStageLabelView != null) mSetupStageLabelView.setText(mSetupStageLabel);
        if (mSetupStageDetailView != null) mSetupStageDetailView.setText(mSetupStageDetail);
    }

    private void updateWifiProgressUi(float fraction, String label, String detail) {
        mWifiProgressFrac = Math.max(0f, Math.min(1f, fraction));
        if (label != null) mWifiStatusLabel = label;
        if (detail != null) mWifiStatusDetail = detail;
        if (mWifiProgressBar != null) {
            mWifiProgressBar.setProgress(Math.round(mWifiProgressFrac * 1000f));
        }
        if (mWifiStatusLabelView != null) mWifiStatusLabelView.setText(mWifiStatusLabel);
        if (mWifiStatusDetailView != null) mWifiStatusDetailView.setText(mWifiStatusDetail);
    }

    private void setBusyState(boolean busy) {
        mBusy.set(busy);
        runOnUiThread(this::refreshButtonsEnabledState);
    }

    private void refreshButtonsEnabledState() {
        boolean busy = mBusy.get();
        if (mBtnHeroPlay != null) mBtnHeroPlay.setEnabled(!busy && mReadyToPlay);
        if (mBtnPickIso != null) mBtnPickIso.setEnabled(!busy);
        if (mBtnPickFolder != null) mBtnPickFolder.setEnabled(!busy);
        if (mBtnRunSetup != null) mBtnRunSetup.setEnabled(!busy && (mHasGameDisc || mPendingIsoUri != null));
        if (mBtnCancelSetup != null) mBtnCancelSetup.setEnabled(busy);
        if (mBtnAddStfs != null) mBtnAddStfs.setEnabled(!busy);
        if (mBtnAddMod != null) mBtnAddMod.setEnabled(!busy);
        if (mBtnBackupSave != null) mBtnBackupSave.setEnabled(!busy);
        if (mBtnImportSaveZip != null) mBtnImportSaveZip.setEnabled(!busy);
        if (mBtnWifiDiscover != null) mBtnWifiDiscover.setEnabled(!busy);
        if (mBtnWifiSync != null) mBtnWifiSync.setEnabled(!busy);
        if (mBtnWifiCancel != null) mBtnWifiCancel.setEnabled(busy);
        if (mBtnImportTurnip != null) mBtnImportTurnip.setEnabled(!busy);
    }

    private void cancelActiveWork() {
        if (!mBusy.get()) return;
        mCancelRequested.set(true);
        try {
            SetupBridge.nativeCancelSetup();
        } catch (Throwable ignored) {
        }
        appendSetupLog("Cancellation requested...");
        appendWifiLog("Cancellation requested...");
        updateSetupProgressUi(mSetupProgressFrac, "Cancelling...", "Waiting for active task to stop");
        updateWifiProgressUi(mWifiProgressFrac, "Cancelling...", "Stopping Wi-Fi transfer");
    }

    // ---- SAF Pickers --------------------------------------------------------

    private void openIsoPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_PICK_ISO);
    }

    private void openFolderPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(i, REQ_PICK_FOLDER);
    }

    private void openStfsPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        i.setType("*/*");
        startActivityForResult(i, REQ_PICK_STFS);
    }

    private void openModZipPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_PICK_MOD_ZIP);
    }

    private void openTurnipPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_PICK_TURNIP);
    }

    private void openSaveZipPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_PICK_SAVE_ZIP);
    }

    private String queryDisplayName(Uri uri) {
        if (uri == null) return "file";
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME },
                null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null && !n.isEmpty()) return n;
            }
        } catch (Throwable ignored) {
        }
        String last = uri.getLastPathSegment();
        return last != null ? last : "file";
    }

    private void tryTakePersistablePermission(Uri uri) {
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Throwable ignored) {
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) return;

        if (requestCode == REQ_PICK_ISO) {
            Uri uri = data.getData();
            if (uri == null) return;
            tryTakePersistablePermission(uri);
            mPendingIsoUri = uri;
            mPendingIsoName = queryDisplayName(uri);
            if (mDiscStatusText != null) {
                mDiscStatusText.setText("ISO selected: " + mPendingIsoName + " (tap Run Setup to extract & index)");
                mDiscStatusText.setTextColor(COL_WARNING);
            }
            appendSetupLog("Selected ISO: " + mPendingIsoName);
            refreshButtonsEnabledState();
        } else if (requestCode == REQ_PICK_FOLDER) {
            Uri treeUri = data.getData();
            if (treeUri != null) importExtractedFolderAsync(treeUri);
        } else if (requestCode == REQ_PICK_STFS) {
            List<Uri> uris = new ArrayList<>();
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); ++i) {
                    Uri u = clip.getItemAt(i).getUri();
                    if (u != null) uris.add(u);
                }
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
            if (!uris.isEmpty()) installStfsBatchAsync(uris);
        } else if (requestCode == REQ_PICK_MOD_ZIP) {
            Uri uri = data.getData();
            if (uri != null) installModZipAsync(uri);
        } else if (requestCode == REQ_PICK_TURNIP) {
            Uri uri = data.getData();
            if (uri != null) installTurnipAsync(uri);
        } else if (requestCode == REQ_PICK_SAVE_ZIP) {
            Uri uri = data.getData();
            if (uri != null) importSaveZipAsync(uri);
        }
    }

    // ---- Setup Pipeline & Import Tasks --------------------------------------

    private void runSetupPipeline() {
        if (!mBusy.compareAndSet(false, true)) return;
        mCancelRequested.set(false);
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                if (mPendingIsoUri != null) {
                    appendSetupLog("Extracting ISO: " + mPendingIsoName);
                    ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(mPendingIsoUri, "r");
                    if (pfd == null) {
                        appendSetupLog("ERROR: Unable to open ISO file descriptor");
                        runOnUiThread(() -> updateSetupProgressUi(0f, "Extraction failed", "Cannot open selected .ISO"));
                        return;
                    }
                    int fd = pfd.detachFd();
                    String resJson = SetupBridge.nativeExtractIsoFd(
                            fd, getGameDir().getAbsolutePath(), true, true, mBridgeListener);
                    JSONObject res = new JSONObject(resJson);
                    if (!res.optBoolean("ok", false)) {
                        String err = res.optString("error", "ISO extraction failed");
                        runOnUiThread(() -> updateSetupProgressUi(0f, "ISO extraction stopped", err));
                        return;
                    }
                    mPendingIsoUri = null;
                    mPendingIsoName = null;
                }

                appendSetupLog("Building Colorado world index & play.txt (zero-bake direct-from-disc)...");
                String bakeJson = SetupBridge.nativeRunOnDeviceBake(
                        getGameDir().getAbsolutePath(),
                        getStateDir().getAbsolutePath(),
                        getPlayDir().getAbsolutePath(),
                        false,
                        true,
                        mBridgeListener);
                JSONObject bake = new JSONObject(bakeJson);
                if (bake.optBoolean("ok", false)) {
                    int total = bake.optInt("cars_total_available", 0);
                    String detail = total > 0
                            ? total + " car(s) ready (direct from disc) — switch to Play!"
                            : "Direct-from-disc setup ready — switch to Play!";
                    runOnUiThread(() -> updateSetupProgressUi(1f, "Setup complete", detail));
                } else {
                    String err = bake.optString("error", "Setup failed");
                    runOnUiThread(() -> updateSetupProgressUi(0f, "Setup stopped", err));
                }
            } catch (Throwable t) {
                appendSetupLog("ERROR: " + t.getMessage());
                runOnUiThread(() -> updateSetupProgressUi(0f, "Setup error", String.valueOf(t.getMessage())));
            } finally {
                setBusyState(false);
                refreshAllAsync();
            }
        });
    }

    private void importExtractedFolderAsync(Uri treeUri) {
        if (!mBusy.compareAndSet(false, true)) return;
        mCancelRequested.set(false);
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                appendSetupLog("Importing extracted game folder via SAF...");
                String rootDocId = DocumentsContract.getTreeDocumentId(treeUri);
                String rootName = rootDocId;
                int slash = rootName.lastIndexOf('/');
                if (slash >= 0) rootName = rootName.substring(slash + 1);
                boolean isMediaRoot = "media".equalsIgnoreCase(rootName);
                File targetBase = isMediaRoot ? new File(getGameDir(), "media") : getGameDir();
                targetBase.mkdirs();

                int[] copied = new int[] { 0 };
                copyDocumentTreeRecursive(treeUri, rootDocId, targetBase, copied);
                if (mCancelRequested.get()) {
                    appendSetupLog("Folder import cancelled after " + copied[0] + " file(s).");
                } else {
                    appendSetupLog("Imported " + copied[0] + " file(s) into " + targetBase.getAbsolutePath());
                    runOnUiThread(() -> updateSetupProgressUi(1f, "Folder imported",
                            copied[0] + " files copied — tap Run Setup to index"));
                }
            } catch (Throwable t) {
                appendSetupLog("Folder import error: " + t.getMessage());
                runOnUiThread(() -> updateSetupProgressUi(0f, "Folder import failed", String.valueOf(t.getMessage())));
            } finally {
                setBusyState(false);
                refreshAllAsync();
            }
        });
    }

    private void copyDocumentTreeRecursive(Uri treeUri, String parentDocId, File outDir, int[] copied) throws Exception {
        if (mCancelRequested.get()) return;
        ContentResolver cr = getContentResolver();
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId);
        String[] cols = new String[] {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE
        };
        try (Cursor c = cr.query(childrenUri, cols, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                if (mCancelRequested.get()) return;
                String docId = c.getString(0);
                String name = c.getString(1);
                String mime = c.getString(2);
                long size = c.isNull(3) ? -1L : c.getLong(3);
                if (name == null || name.isEmpty() || name.contains("..")) continue;

                File dest = new File(outDir, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    dest.mkdirs();
                    copyDocumentTreeRecursive(treeUri, docId, dest, copied);
                } else {
                    String lower = name.toLowerCase(Locale.ROOT);
                    if (lower.endsWith(".xex") || lower.endsWith(".xexp")) continue;
                    if (dest.isFile() && size >= 0 && dest.length() == size) {
                        copied[0]++;
                        continue;
                    }
                    Uri docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
                    runOnUiThread(() -> updateSetupProgressUi(0.5f, "Importing game folder", name));
                    File tmp = new File(dest.getParentFile(), dest.getName() + ".part");
                    try (InputStream in = cr.openInputStream(docUri);
                         FileOutputStream out = new FileOutputStream(tmp)) {
                        if (in == null) continue;
                        byte[] buf = new byte[262144];
                        int n;
                        while ((n = in.read(buf)) > 0) {
                            if (mCancelRequested.get()) {
                                tmp.delete();
                                return;
                            }
                            out.write(buf, 0, n);
                        }
                    }
                    if (dest.exists()) dest.delete();
                    tmp.renameTo(dest);
                    copied[0]++;
                }
            }
        }
    }

    private void installStfsBatchAsync(List<Uri> uris) {
        if (!mBusy.compareAndSet(false, true)) return;
        mCancelRequested.set(false);
        setBusyState(true);
        mExecutor.execute(() -> {
            int installed = 0;
            try {
                for (int i = 0; i < uris.size(); ++i) {
                    Uri u = uris.get(i);
                    String name = queryDisplayName(u);
                    appendSetupLog("Installing STFS [" + (i + 1) + "/" + uris.size() + "]: " + name);
                    ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(u, "r");
                    if (pfd == null) {
                        appendSetupLog("Skipped unreadable file: " + name);
                        continue;
                    }
                    int fd = pfd.detachFd();
                    String json = SetupBridge.nativeInstallStfsFd(
                            fd, name, getPlayDir().getAbsolutePath(), getStateDir().getAbsolutePath(), mBridgeListener);
                    JSONObject obj = new JSONObject(json);
                    if (obj.optBoolean("ok", false)) {
                        ++installed;
                    } else if (obj.optBoolean("cancelled", false)) {
                        break;
                    }
                }
                final int count = installed;
                runOnUiThread(() -> updateSetupProgressUi(1f, "Updates & DLC updated",
                        "Installed " + count + " package(s)."));
            } catch (Throwable t) {
                appendSetupLog("STFS install error: " + t.getMessage());
            } finally {
                setBusyState(false);
                refreshAllAsync();
            }
        });
    }

    private void installModZipAsync(Uri uri) {
        if (!mBusy.compareAndSet(false, true)) return;
        mCancelRequested.set(false);
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                String name = queryDisplayName(uri);
                ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
                if (pfd == null) {
                    appendSetupLog("Cannot open mod archive: " + name);
                    return;
                }
                int fd = pfd.detachFd();
                String json = SetupBridge.nativeInstallModZipFd(
                        fd, name, getModsDir().getAbsolutePath(), mBridgeListener);
                JSONObject obj = new JSONObject(json);
                if (obj.optBoolean("ok", false)) {
                    String modName = obj.optString("mod_name", name);
                    runOnUiThread(() -> updateSetupProgressUi(1f, "Mod installed", modName));
                } else {
                    String err = obj.optString("error", "Mod install failed");
                    runOnUiThread(() -> updateSetupProgressUi(0f, "Mod install failed", err));
                }
            } catch (Throwable t) {
                appendSetupLog("Mod install error: " + t.getMessage());
            } finally {
                setBusyState(false);
                refreshAllAsync();
            }
        });
    }

    private void installTurnipAsync(Uri uri) {
        if (!mBusy.compareAndSet(false, true)) return;
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                String name = queryDisplayName(uri);
                ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
                if (pfd == null) {
                    appendSetupLog("Cannot open driver file: " + name);
                    return;
                }
                int fd = pfd.detachFd();
                String json = SetupBridge.nativeInstallTurnipFd(fd, name, getDriversDir().getAbsolutePath());
                JSONObject obj = new JSONObject(json);
                if (obj.optBoolean("ok", false)) {
                    appendSetupLog("Installed Turnip driver: " + obj.optString("driver_name", name));
                } else {
                    appendSetupLog("Turnip import error: " + obj.optString("error", "Driver import failed"));
                }
            } catch (Throwable t) {
                appendSetupLog("Turnip import error: " + t.getMessage());
            } finally {
                setBusyState(false);
                refreshAllAsync();
            }
        });
    }

    // ---- Save Management Tasks ----------------------------------------------

    private void backupSaveAsync() {
        if (!mBusy.compareAndSet(false, true)) return;
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                String json = SetupBridge.nativeBackupSave(getFilesRoot().getAbsolutePath());
                JSONObject obj = new JSONObject(json);
                final boolean ok = obj.optBoolean("ok", false);
                final boolean skipped = obj.optBoolean("skipped", false);
                final String msg = ok
                        ? "Backup snapshot created: " + obj.optString("id", "")
                        : (skipped ? "No save file to back up yet." : "Backup failed: " + obj.optString("error", ""));
                runOnUiThread(() -> showSaveBanner(msg, ok ? COL_SUCCESS : COL_WARNING));
            } catch (Throwable t) {
                runOnUiThread(() -> showSaveBanner("Backup error: " + t.getMessage(), COL_DANGER));
            } finally {
                setBusyState(false);
                refreshSavesAsync();
            }
        });
    }

    private void importSaveZipAsync(Uri uri) {
        if (!mBusy.compareAndSet(false, true)) return;
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                String name = queryDisplayName(uri);
                ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
                if (pfd == null) {
                    runOnUiThread(() -> showSaveBanner("Cannot open selected save archive.", COL_DANGER));
                    return;
                }
                int fd = pfd.detachFd();
                String json = SetupBridge.nativeImportSaveZipFd(
                        fd, name, getFilesRoot().getAbsolutePath(), getStateDir().getAbsolutePath());
                JSONObject obj = new JSONObject(json);
                if (obj.optBoolean("ok", false)) {
                    long credits = obj.optLong("credits", 0L);
                    int cars = obj.optInt("garage_cars", 0);
                    int signs = obj.optInt("signs", 0);
                    String msg = "Imported save: " + formatCredits(credits) + "  •  "
                            + cars + " cars  •  " + signs + " signs";
                    runOnUiThread(() -> showSaveBanner(msg, COL_SUCCESS));
                } else {
                    String err = obj.optString("error", "Import failed");
                    runOnUiThread(() -> showSaveBanner("Import failed: " + err, COL_DANGER));
                }
            } catch (Throwable t) {
                runOnUiThread(() -> showSaveBanner("Import error: " + t.getMessage(), COL_DANGER));
            } finally {
                setBusyState(false);
                refreshSavesAsync();
            }
        });
    }

    private void showSaveBanner(String text, int color) {
        if (mSaveStatusBanner == null) return;
        mSaveStatusBanner.setText(text);
        mSaveStatusBanner.setTextColor(color);
        mSaveStatusBanner.setVisibility(View.VISIBLE);
    }

    // ---- Wi-Fi Sync Tasks ---------------------------------------------------

    private void discoverWifiHostAsync() {
        if (!mBusy.compareAndSet(false, true)) return;
        setBusyState(true);
        runOnUiThread(() -> updateWifiProgressUi(0.15f, "Scanning LAN...", "Broadcasting discovery probe on UDP 47616"));
        appendWifiLog("Broadcasting UDP discovery probe on port " + WifiSyncClient.DISCOVERY_PORT + "...");
        mExecutor.execute(() -> {
            try {
                WifiSyncClient.DiscoveredHost host = WifiSyncClient.discoverOnLan(3500);
                if (host != null) {
                    mWifiHostVal = host.ip;
                    mWifiPortVal = String.valueOf(host.port);
                    appendWifiLog("Found PC: " + host.name + " (" + host.ip + ":" + host.port + ")");
                    runOnUiThread(() -> {
                        if (mEditWifiHost != null) mEditWifiHost.setText(host.ip);
                        if (mEditWifiPort != null) mEditWifiPort.setText(String.valueOf(host.port));
                        updateWifiProgressUi(0f, "PC discovered: " + host.name,
                                "Enter the 6-digit pairing code shown on " + host.ip + " and tap Sync Now.");
                    });
                } else {
                    appendWifiLog("No PC responded on LAN. Verify Shift Engine Launcher Wi-Fi Sync is open.");
                    runOnUiThread(() -> updateWifiProgressUi(0f, "No PC found on LAN",
                            "Enter the Host IP, Port, and 6-digit Pairing Code manually."));
                }
            } catch (Throwable t) {
                appendWifiLog("Discovery error: " + t.getMessage());
                runOnUiThread(() -> updateWifiProgressUi(0f, "Discovery error", String.valueOf(t.getMessage())));
            } finally {
                setBusyState(false);
            }
        });
    }

    private void runWifiSyncAsync() {
        captureWifiInputs();
        final String host = mWifiHostVal;
        final String code = mWifiCodeVal;
        int parsedPort = WifiSyncClient.DISCOVERY_PORT;
        try {
            if (!mWifiPortVal.isEmpty()) parsedPort = Integer.parseInt(mWifiPortVal);
        } catch (NumberFormatException ignored) {
        }
        final int port = parsedPort;

        if (host.isEmpty()) {
            updateWifiProgressUi(0f, "Host IP required", "Tap 'Discover PC on LAN' or type your PC's IP address.");
            return;
        }
        if (code.length() != 6) {
            updateWifiProgressUi(0f, "Pairing code required", "Enter the 6-digit pairing code shown on your PC.");
            return;
        }
        if (!mBusy.compareAndSet(false, true)) return;
        mCancelRequested.set(false);
        setBusyState(true);

        mExecutor.execute(() -> {
            try {
                appendWifiLog("Connecting to http://" + host + ":" + port + "/shift/v1/manifest ...");
                runOnUiThread(() -> updateWifiProgressUi(0.05f, "Fetching manifest...", host + ":" + port));

                String manifestStr = WifiSyncClient.fetchManifest(host, port, code);
                JSONObject manifest = new JSONObject(manifestStr);
                String pcName = manifest.optString("name", host);
                JSONArray groups = manifest.optJSONArray("groups");

                List<JSONObject> filesToSync = new ArrayList<>();
                long totalBytes = 0L;
                if (groups != null) {
                    for (int gi = 0; gi < groups.length(); ++gi) {
                        JSONObject g = groups.optJSONObject(gi);
                        if (g == null || !g.optBoolean("selected", true)) continue;
                        JSONArray files = g.optJSONArray("files");
                        if (files == null) continue;
                        for (int fi = 0; fi < files.length(); ++fi) {
                            JSONObject f = files.optJSONObject(fi);
                            if (f != null) {
                                filesToSync.add(f);
                                totalBytes += Math.max(0L, f.optLong("size", 0L));
                            }
                        }
                    }
                }

                appendWifiLog("Paired with " + pcName + ": " + filesToSync.size() + " file(s) ("
                        + formatSizeCompact(totalBytes) + ")");
                if (filesToSync.isEmpty()) {
                    runOnUiThread(() -> updateWifiProgressUi(1f, "Nothing to sync", "PC manifest contained 0 files."));
                    return;
                }

                File filesRoot = getFilesRoot();
                long doneBytes = 0L;
                int doneFiles = 0;
                for (int i = 0; i < filesToSync.size(); ++i) {
                    if (mCancelRequested.get()) {
                        final int completedNow = doneFiles;
                        appendWifiLog("Wi-Fi Sync cancelled at " + completedNow + "/" + filesToSync.size() + " files.");
                        runOnUiThread(() -> updateWifiProgressUi(0f, "Sync cancelled",
                                "Completed " + completedNow + " of " + filesToSync.size() + " file(s)."));
                        return;
                    }
                    JSONObject f = filesToSync.get(i);
                    int fileIdx = f.optInt("index", i);
                    String relPath = f.optString("path", "");
                    long sz = Math.max(0L, f.optLong("size", 0L));
                    if (relPath.isEmpty()) continue;

                    // Route mods/ into state/mods/ so Android DriveActivity finds synced mods seamlessly
                    String targetRel = relPath.startsWith("mods/") ? ("state/" + relPath) : relPath;
                    final float frac = totalBytes > 0
                            ? (float) doneBytes / (float) totalBytes
                            : (float) i / (float) filesToSync.size();
                    final int fileNum = i + 1;
                    final int totalFiles = filesToSync.size();
                    final String label = "Syncing (" + fileNum + "/" + totalFiles + ")";
                    runOnUiThread(() -> updateWifiProgressUi(frac, label, targetRel));

                    WifiSyncClient.downloadFileResumable(host, port, code, fileIdx, filesRoot, targetRel, sz);
                    doneBytes += sz;
                    doneFiles++;
                }

                appendWifiLog("Wi-Fi Sync complete: " + doneFiles + " file(s) (" + formatSizeCompact(doneBytes) + ").");
                final int finalCount = doneFiles;
                final long finalBytes = doneBytes;
                runOnUiThread(() -> updateWifiProgressUi(1f, "Wi-Fi Sync complete",
                        finalCount + " files synced (" + formatSizeCompact(finalBytes) + ") — ready to play!"));
            } catch (Throwable t) {
                appendWifiLog("Wi-Fi Sync error: " + t.getMessage());
                runOnUiThread(() -> updateWifiProgressUi(0f, "Wi-Fi Sync failed", String.valueOf(t.getMessage())));
            } finally {
                setBusyState(false);
                refreshAllAsync();
            }
        });
    }

    // ---- Library & Save Inspection ------------------------------------------

    private void refreshAllAsync() {
        refreshLibraryAsync();
        refreshSavesAsync();
    }

    private void refreshLibraryAsync() {
        mExecutor.execute(() -> {
            try {
                String json = SetupBridge.nativeInspectLibrary(
                        getPlayDir().getAbsolutePath(), getStateDir().getAbsolutePath());
                JSONObject st = new JSONObject(json);
                mLastLibraryJson = st;
                runOnUiThread(() -> applyLibraryStatus(st));
            } catch (Throwable t) {
                appendSetupLog("Inspect error: " + t.getMessage());
            }
        });
    }

    private void refreshSavesAsync() {
        mExecutor.execute(() -> {
            JSONObject obj = null;
            try {
                String json = SetupBridge.nativeInspectSaves(getFilesRoot().getAbsolutePath());
                obj = new JSONObject(json);
            } catch (Throwable ignored) {
                // Graceful fallback parser if JNI library is unavailable
                File saveTxt = LauncherSettingsStore.resolveActiveSaveFile(getFilesRoot());
                LauncherSettingsStore.FallbackSaveInfo fb = LauncherSettingsStore.parseSaveFileFallback(saveTxt);
                try {
                    obj = new JSONObject();
                    obj.put("active_profile", "default");
                    obj.put("gamertag", "Player");
                    obj.put("has_save", fb.hasSave);
                    obj.put("car", fb.car);
                    obj.put("credits", fb.credits);
                    obj.put("garage_cars", fb.garageCars);
                    obj.put("signs_found", fb.signsFound);
                    obj.put("level", fb.level);
                } catch (Throwable ignored2) {
                }
            }
            if (obj != null) {
                mLastSavesJson = obj;
                final JSONObject finalObj = obj;
                runOnUiThread(() -> applySavesStatus(finalObj));
            }
        });
    }

    private void applyLibraryStatus(JSONObject st) {
        mHasGameDisc = st.optBoolean("has_game_disc", false);
        mReadyToPlay = st.optBoolean("ready_to_play", false);
        mDiscCarCount = st.optInt("disc_car_count", 0);
        mHasV4 = st.optBoolean("has_v4", false);
        mHasTurnip = st.optBoolean("has_turnip_driver", false);
        mTurnipPath = st.optString("turnip_driver_path", "");

        mFreeStorageBytes = st.optLong("free_storage_bytes", 0L);
        if (mFreeStorageBytes <= 0L) {
            try {
                StatFs sf = new StatFs(getFilesRoot().getAbsolutePath());
                mFreeStorageBytes = sf.getAvailableBytes();
            } catch (Throwable ignored) {
            }
        }

        // Count DLC & Mods
        JSONArray pkgs = st.optJSONArray("packages");
        mDlcCount = 0;
        if (pkgs != null) {
            for (int i = 0; i < pkgs.length(); ++i) {
                JSONObject p = pkgs.optJSONObject(i);
                if (p != null && !"title_update".equalsIgnoreCase(p.optString("kind", ""))) {
                    mDlcCount++;
                }
            }
        }

        JSONArray mods = st.optJSONArray("mods");
        mModsTotalCount = mods != null ? mods.length() : 0;
        mModsEnabledCount = 0;
        if (mods != null) {
            for (int i = 0; i < mods.length(); ++i) {
                JSONObject m = mods.optJSONObject(i);
                if (m != null && m.optBoolean("enabled", true)) mModsEnabledCount++;
            }
        }

        // Status pill (rail / top bar)
        if (mStatusPillLabel != null && mStatusPillDot != null) {
            if (mReadyToPlay) {
                mStatusPillLabel.setText("Game Ready");
                mStatusPillDot.setBackground(makeCircleDrawable(COL_SUCCESS));
            } else {
                mStatusPillLabel.setText("Setup Needed");
                mStatusPillDot.setBackground(makeCircleDrawable(COL_WARNING));
            }
        }
        if (mStatusStorageLabel != null) {
            mStatusStorageLabel.setText(formatFreeStorage(mFreeStorageBytes));
        }

        // Hero Card (Tab 1)
        if (mHeroStatusDot != null && mHeroStatusText != null) {
            if (mReadyToPlay) {
                mHeroStatusDot.setBackground(makeCircleDrawable(COL_SUCCESS));
                mHeroStatusText.setText("Ready to Drive  •  " + mDiscCarCount + " Cars Available");
                mHeroStatusText.setTextColor(COL_TEXT_PRIMARY);
            } else if (mHasGameDisc || mPendingIsoUri != null) {
                mHeroStatusDot.setBackground(makeCircleDrawable(COL_WARNING));
                mHeroStatusText.setText("Game Disc Found  •  Run Setup to Build World Index");
                mHeroStatusText.setTextColor(COL_WARNING);
            } else {
                mHeroStatusDot.setBackground(makeCircleDrawable(COL_WARNING));
                mHeroStatusText.setText("Setup Needed  •  Import Game Disc or Sync via Wi-Fi");
                mHeroStatusText.setTextColor(COL_TEXT_SECONDARY);
            }
        }
        if (mBtnHeroGoSetup != null) {
            mBtnHeroGoSetup.setVisibility(mReadyToPlay ? View.GONE : View.VISIBLE);
        }

        // Library Summary tiles (Tab 1)
        if (mSummaryV4Val != null) {
            mSummaryV4Val.setText(mHasV4 ? "Installed" : "Not Installed");
            mSummaryV4Val.setTextColor(mHasV4 ? COL_SUCCESS : COL_TEXT_MUTED);
        }
        if (mSummaryDlcVal != null) {
            mSummaryDlcVal.setText(mDlcCount + " pack" + (mDlcCount == 1 ? "" : "s"));
            mSummaryDlcVal.setTextColor(mDlcCount > 0 ? COL_TEXT_PRIMARY : COL_TEXT_MUTED);
        }
        if (mSummaryModsVal != null) {
            mSummaryModsVal.setText(mModsEnabledCount + " / " + mModsTotalCount + " active");
            mSummaryModsVal.setTextColor(mModsEnabledCount > 0 ? COL_ACCENT_FOCUS : COL_TEXT_MUTED);
        }
        updateDriverSummaryText();

        // Tab 2: Disc status
        if (mDiscStatusText != null) {
            if (mPendingIsoUri != null) {
                mDiscStatusText.setText("Queued ISO: " + mPendingIsoName + " (ready to extract & index)");
                mDiscStatusText.setTextColor(COL_WARNING);
            } else if (mHasGameDisc) {
                mDiscStatusText.setText("Verified Forza Horizon 1 disc (" + mDiscCarCount + " car archives)");
                mDiscStatusText.setTextColor(COL_SUCCESS);
            } else {
                mDiscStatusText.setText("No game disc installed yet");
                mDiscStatusText.setTextColor(COL_WARNING);
            }
        }
        if (mDiscHintText != null) {
            mDiscHintText.setText(formatFreeStorage(mFreeStorageBytes) + "  •  " + getGameDir().getAbsolutePath());
        }

        // Tab 3: Packages list
        if (mPackagesList != null) {
            mPackagesList.removeAllViews();
            if (pkgs == null || pkgs.length() == 0) {
                TextView empty = new TextView(this);
                empty.setText("No updates or DLC packs installed yet.");
                empty.setTextColor(COL_TEXT_MUTED);
                empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
                mPackagesList.addView(empty);
            } else {
                for (int i = 0; i < pkgs.length(); ++i) {
                    JSONObject p = pkgs.optJSONObject(i);
                    if (p == null) continue;
                    mPackagesList.addView(makePackageRow(p));
                }
            }
        }

        // Tab 3: Mods list
        if (mModsList != null) {
            mModsList.removeAllViews();
            if (mods == null || mods.length() == 0) {
                TextView empty = new TextView(this);
                empty.setText("No mods installed yet.");
                empty.setTextColor(COL_TEXT_MUTED);
                empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
                mModsList.addView(empty);
            } else {
                for (int i = 0; i < mods.length(); ++i) {
                    JSONObject m = mods.optJSONObject(i);
                    if (m == null) continue;
                    mModsList.addView(makeModCardRow(m));
                }
            }
        }

        refreshButtonsEnabledState();
    }

    private void updateDriverSummaryText() {
        String pref = LauncherSettingsStore.getOrDefault(mSettings, LauncherSettingsStore.KEY_GPU_DRIVER, "auto");
        boolean usingTurnip = mHasTurnip && !"system".equals(pref);
        if (mSummaryDriverVal != null) {
            if ("system".equals(pref)) {
                mSummaryDriverVal.setText("System Vulkan");
                mSummaryDriverVal.setTextColor(COL_TEXT_SECONDARY);
            } else if (mHasTurnip) {
                mSummaryDriverVal.setText("Mesa Turnip");
                mSummaryDriverVal.setTextColor(COL_SUCCESS);
            } else {
                mSummaryDriverVal.setText("System (Auto)");
                mSummaryDriverVal.setTextColor(COL_TEXT_SECONDARY);
            }
        }
        if (mTurnipStatusText != null) {
            if (usingTurnip) {
                mTurnipStatusText.setText("Active Vulkan driver: Custom Mesa Turnip");
                mTurnipStatusText.setTextColor(COL_SUCCESS);
            } else if (mHasTurnip && "system".equals(pref)) {
                mTurnipStatusText.setText("Active Vulkan driver: System (Turnip installed, disabled in preference)");
                mTurnipStatusText.setTextColor(COL_WARNING);
            } else {
                mTurnipStatusText.setText("Active Vulkan driver: Android System Default");
                mTurnipStatusText.setTextColor(COL_TEXT_SECONDARY);
            }
        }
        if (mTurnipPathText != null) {
            mTurnipPathText.setText(mHasTurnip && !mTurnipPath.isEmpty()
                    ? mTurnipPath
                    : "No custom Turnip .so imported yet.");
        }
    }

    private View makePackageRow(JSONObject p) {
        LinearLayout row = makeHRow();
        row.setBackground(makeCardDrawable(COL_INSET, COL_BORDER, 8));
        row.setPadding(dp(10), dp(8), dp(12), dp(8));
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rlp.bottomMargin = dp(6);
        row.setLayoutParams(rlp);

        String kind = p.optString("kind", "dlc");
        String tag = kind.toUpperCase(Locale.ROOT).replace('_', ' ');
        int badgeColor = "title_update".equalsIgnoreCase(kind) ? COL_ACCENT
                : ("rally_expansion".equalsIgnoreCase(kind) ? COL_SUCCESS : COL_WARNING);
        int badgeFill = "title_update".equalsIgnoreCase(kind) ? COL_ACCENT_SOFT
                : ("rally_expansion".equalsIgnoreCase(kind) ? COL_SUCCESS_SOFT : COL_WARNING_SOFT);

        TextView badge = new TextView(this);
        badge.setText(tag);
        badge.setTextColor(badgeColor);
        badge.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f);
        badge.setTypeface(Typeface.DEFAULT_BOLD);
        badge.setBackground(makePillDrawable(badgeFill, badgeColor, 6));
        badge.setPadding(dp(7), dp(3), dp(7), dp(3));
        setRightMargin(badge, 10);
        row.addView(badge);

        TextView name = new TextView(this);
        name.setText(p.optString("name", p.optString("id", "Package")));
        name.setTextColor(COL_TEXT_PRIMARY);
        name.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        name.setTypeface(Typeface.DEFAULT_BOLD);
        row.addView(name);
        return row;
    }

    private View makeModCardRow(JSONObject m) {
        final String modName = m.optString("name", "");
        final String modTitle = m.optString("title", modName);
        final boolean enabled = m.optBoolean("enabled", true);
        final String summary = m.optString("summary", "");

        LinearLayout card = makeHRow();
        card.setBackground(makeCardDrawable(COL_INSET, enabled ? COL_BORDER : Color.parseColor("#1E2230"), 9));
        card.setPadding(dp(10), dp(8), dp(10), dp(8));
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        clp.bottomMargin = dp(8);
        card.setLayoutParams(clp);

        CheckBox cb = new CheckBox(this);
        cb.setText(modTitle + (summary.isEmpty() ? "" : "\n" + summary));
        cb.setChecked(enabled);
        cb.setTextColor(enabled ? COL_TEXT_PRIMARY : COL_TEXT_MUTED);
        cb.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        cb.setButtonTintList(ColorStateList.valueOf(enabled ? COL_ACCENT : COL_TEXT_MUTED));
        cb.setFocusable(true);
        cb.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        cb.setOnCheckedChangeListener((buttonView, isChecked) -> mExecutor.execute(() -> {
            SetupBridge.nativeSetModEnabled(getModsDir().getAbsolutePath(), modName, isChecked);
            refreshLibraryAsync();
        }));
        card.addView(cb);

        Button up = makeSecondaryButton("Up", v -> mExecutor.execute(() -> {
            SetupBridge.nativeMoveMod(getModsDir().getAbsolutePath(), modName, -1);
            refreshLibraryAsync();
        }));
        setRightMargin(up, 6);
        card.addView(up);

        Button down = makeSecondaryButton("Down", v -> mExecutor.execute(() -> {
            SetupBridge.nativeMoveMod(getModsDir().getAbsolutePath(), modName, 1);
            refreshLibraryAsync();
        }));
        setRightMargin(down, 6);
        card.addView(down);

        Button del = makeDangerButton("Remove", v -> mExecutor.execute(() -> {
            SetupBridge.nativeRemoveMod(getModsDir().getAbsolutePath(), modName);
            refreshLibraryAsync();
        }));
        card.addView(del);
        return card;
    }

    private void applySavesStatus(JSONObject s) {
        String gamertag = s.optString("gamertag", "Player");
        String profileName = s.optString("active_profile", "default");
        boolean hasSave = s.optBoolean("has_save", false);
        long credits = s.optLong("credits", 0L);
        int garageCars = s.optInt("garage_cars", 0);
        int signs = s.optInt("signs_found", 0);
        String car = s.optString("car", "");

        if (mHeroProfileText != null) {
            if (hasSave) {
                mHeroProfileText.setText("Profile: " + gamertag + "  •  "
                        + formatCredits(credits) + "  •  " + garageCars + " garage car" + (garageCars == 1 ? "" : "s"));
            } else {
                mHeroProfileText.setText("Profile: " + gamertag + "  •  Ready to start new career");
            }
        }

        if (mSaveProfileTitle != null) {
            mSaveProfileTitle.setText("Active Profile: " + gamertag + " (" + profileName + ")");
        }
        if (mSaveMetaCredits != null) {
            mSaveMetaCredits.setText(hasSave ? formatCredits(credits) : "No save yet");
        }
        if (mSaveMetaGarage != null) {
            mSaveMetaGarage.setText(hasSave ? (garageCars + " car" + (garageCars == 1 ? "" : "s")) : "0");
        }
        if (mSaveMetaSigns != null) {
            mSaveMetaSigns.setText(hasSave ? (signs + " / 100") : "0 / 100");
        }
        if (mSaveMetaCar != null) {
            mSaveMetaCar.setText(hasSave && !car.isEmpty() ? car : "None");
        }

        if (mBackupsList != null) {
            mBackupsList.removeAllViews();
            JSONArray backups = s.optJSONArray("backups");
            if (backups == null || backups.length() == 0) {
                TextView empty = new TextView(this);
                empty.setText("No rolling backups yet. Tap 'Backup Save' to create one.");
                empty.setTextColor(COL_TEXT_MUTED);
                empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
                mBackupsList.addView(empty);
            } else {
                for (int i = 0; i < backups.length(); ++i) {
                    JSONObject b = backups.optJSONObject(i);
                    if (b == null) continue;
                    LinearLayout row = makeHRow();
                    row.setBackground(makeCardDrawable(COL_INSET, COL_BORDER, 8));
                    row.setPadding(dp(12), dp(8), dp(12), dp(8));
                    LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                    rlp.bottomMargin = dp(6);
                    row.setLayoutParams(rlp);

                    LinearLayout left = new LinearLayout(this);
                    left.setOrientation(LinearLayout.VERTICAL);
                    left.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

                    TextView title = new TextView(this);
                    title.setText(b.optString("time_str", b.optString("id", "")) + "  •  " + b.optString("label", "Snapshot"));
                    title.setTextColor(COL_TEXT_PRIMARY);
                    title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
                    title.setTypeface(Typeface.DEFAULT_BOLD);
                    left.addView(title);

                    TextView sub = new TextView(this);
                    String bCar = b.optString("car", "");
                    long bCr = b.optLong("credits", 0L);
                    sub.setText((bCar.isEmpty() ? "Career save" : bCar) + "  •  " + formatCredits(bCr));
                    sub.setTextColor(COL_TEXT_MUTED);
                    sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
                    left.addView(sub);
                    row.addView(left);
                    mBackupsList.addView(row);
                }
            }
        }
    }

    // ---- Formatting Helpers -------------------------------------------------

    private String formatFreeStorage(long bytes) {
        if (bytes <= 0L) return "Storage unknown";
        double gb = bytes / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1.0) return String.format(Locale.US, "%.1f GB free", gb);
        double mb = bytes / (1024.0 * 1024.0);
        return String.format(Locale.US, "%.0f MB free", mb);
    }

    private String formatSizeCompact(long bytes) {
        if (bytes >= (1L << 30)) return String.format(Locale.US, "%.2f GB", bytes / (double) (1L << 30));
        if (bytes >= (1L << 20)) return String.format(Locale.US, "%.1f MB", bytes / (double) (1L << 20));
        if (bytes >= (1L << 10)) return String.format(Locale.US, "%.0f KB", bytes / (double) (1L << 10));
        return bytes + " B";
    }

    private String formatCredits(long credits) {
        return NumberFormat.getIntegerInstance(Locale.US).format(credits) + " CR";
    }

    // ---- Seamless Launch Into DriveActivity ---------------------------------

    private void launchDriveWithAnimation() {
        if (mLaunchingDrive || !mReadyToPlay || mBusy.get()) return;
        mLaunchingDrive = true;
        LauncherSettingsStore.writeAll(getSettingsFile(), mSettings);

        Runnable doLaunch = () -> {
            Intent drive = new Intent(SetupActivity.this, DriveActivity.class);
            drive.putExtra(DriveActivity.EXTRA_FORCE_PLAY, true);
            drive.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            startActivity(drive);
            LauncherSettingsStore.applyCrossFadeTransition(SetupActivity.this, false);
        };

        if (mBtnHeroPlay != null) {
            mBtnHeroPlay.animate().scaleX(0.96f).scaleY(0.96f).setDuration(80).withEndAction(() -> {
                mBtnHeroPlay.setScaleX(1f);
                mBtnHeroPlay.setScaleY(1f);
                doLaunch.run();
            }).start();
        } else {
            doLaunch.run();
        }
    }
}
