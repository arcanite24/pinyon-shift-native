// Standalone on-device Setup, Content, Mods & GPU Driver UI for Shift Drive.
// Zero PC required: extracts Forza Horizon 1 .iso (or imports an extracted
// folder), installs Title Update v4 / DLC STFS packages, manages .zip mods,
// imports Mesa Turnip drivers, and runs the in-process baker on-device.
// Fully navigable by touch AND gamepad (D-pad / A / B / Y / Start).
package net.shiftengine.drive;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
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

    // Dark high-contrast handheld palette
    private static final int COL_BG = Color.parseColor("#0D1117");
    private static final int COL_CARD = Color.parseColor("#161B22");
    private static final int COL_CARD_BORDER = Color.parseColor("#2A3241");
    private static final int COL_BTN = Color.parseColor("#21262D");
    private static final int COL_BTN_FOCUS = Color.parseColor("#2D3748");
    private static final int COL_ACCENT = Color.parseColor("#F59E0B");
    private static final int COL_ACCENT_DARK = Color.parseColor("#B45309");
    private static final int COL_OK = Color.parseColor("#22C55E");
    private static final int COL_OK_DARK = Color.parseColor("#15803D");
    private static final int COL_ERR = Color.parseColor("#EF4444");
    private static final int COL_TEXT = Color.parseColor("#F1F5F9");
    private static final int COL_MUTED = Color.parseColor("#94A3B8");
    private static final int COL_LOG_BG = Color.parseColor("#090C10");

    private final ExecutorService mExecutor = Executors.newSingleThreadExecutor();
    private final AtomicBoolean mBusy = new AtomicBoolean(false);
    private final AtomicBoolean mFolderCopyCancel = new AtomicBoolean(false);
    private final StringBuilder mLogBuf = new StringBuilder();

    private Uri mPendingIsoUri = null;
    private String mPendingIsoName = null;
    private boolean mFullRosterMode = false;
    private boolean mLogVisible = false;
    private boolean mReadyToPlay = false;
    private boolean mHasGameDisc = false;

    // UI widgets updated on state refresh
    private TextView mHeaderBadge;
    private TextView mStorageText;
    private TextView mDiscStatusText;
    private TextView mDiscHintText;
    private LinearLayout mPackagesList;
    private LinearLayout mModsList;
    private TextView mTurnipStatusText;
    private CheckBox mFullRosterCheck;
    private ProgressBar mProgressBar;
    private TextView mStageLabel;
    private TextView mStageDetail;
    private Button mBtnPickIso;
    private Button mBtnPickFolder;
    private Button mBtnAddDlc;
    private Button mBtnAddMod;
    private Button mBtnImportTurnip;
    private Button mBtnRunSetup;
    private Button mBtnBakeFull;
    private Button mBtnCancel;
    private Button mBtnPlay;
    private Button mBtnToggleLog;
    private ScrollView mLogScroll;
    private TextView mLogText;

    private final SetupBridge.ProgressListener mBridgeListener = new SetupBridge.ProgressListener() {
        @Override
        public void onProgress(int stageIndex, int stageCount, float fraction, String label, String detail) {
            runOnUiThread(() -> updateProgressUi(fraction, label, detail));
        }

        @Override
        public void onLog(String line) {
            appendLog(line);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ensureDirectories();
        buildUi();
        appendLog("Shift Drive standalone setup ready.");
        appendLog("Play folder: " + getPlayDir().getAbsolutePath());
        refreshLibraryAsync();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (mBusy.get()) {
            mFolderCopyCancel.set(true);
            SetupBridge.nativeCancelSetup();
        }
        mExecutor.shutdownNow();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BUTTON_B || keyCode == KeyEvent.KEYCODE_BACK) {
            if (mBusy.get()) {
                cancelActiveWork();
                return true;
            }
            if (mLogVisible) {
                toggleLogVisibility();
                return true;
            }
        } else if (keyCode == KeyEvent.KEYCODE_BUTTON_Y) {
            toggleLogVisibility();
            return true;
        } else if (keyCode == KeyEvent.KEYCODE_BUTTON_START && mReadyToPlay && !mBusy.get()) {
            launchDrive();
            return true;
        }
        return super.onKeyDown(keyCode, event);
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

    private void ensureDirectories() {
        getPlayDir().mkdirs();
        getGameDir().mkdirs();
        getStateDir().mkdirs();
        getModsDir().mkdirs();
        getDriversDir().mkdirs();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private Drawable makeCardBackground() {
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(COL_CARD);
        gd.setCornerRadius(dp(10));
        gd.setStroke(dp(1), COL_CARD_BORDER);
        return gd;
    }

    private Drawable makeButtonDrawable(int normalColor, int pressedColor, boolean accentBorder) {
        GradientDrawable normal = new GradientDrawable();
        normal.setColor(normalColor);
        normal.setCornerRadius(dp(8));
        normal.setStroke(dp(1), accentBorder ? COL_ACCENT : COL_CARD_BORDER);

        GradientDrawable focused = new GradientDrawable();
        focused.setColor(pressedColor);
        focused.setCornerRadius(dp(8));
        focused.setStroke(dp(3), COL_ACCENT);

        GradientDrawable disabled = new GradientDrawable();
        disabled.setColor(Color.parseColor("#1A1F27"));
        disabled.setCornerRadius(dp(8));
        disabled.setStroke(dp(1), Color.parseColor("#232A36"));

        StateListDrawable sld = new StateListDrawable();
        sld.addState(new int[] { -android.R.attr.state_enabled }, disabled);
        sld.addState(new int[] { android.R.attr.state_pressed }, focused);
        sld.addState(new int[] { android.R.attr.state_focused }, focused);
        sld.addState(new int[] {}, normal);
        return sld;
    }

    private Button makeButton(String text, int bg, int focusBg, boolean accentBorder, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(COL_TEXT);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(makeButtonDrawable(bg, focusBg, accentBorder));
        b.setPadding(dp(14), dp(9), dp(14), dp(9));
        b.setMinimumHeight(dp(40));
        b.setMinHeight(dp(40));
        b.setFocusable(true);
        b.setFocusableInTouchMode(false);
        b.setOnClickListener(listener);
        return b;
    }

    private LinearLayout makeCard(String title, String subtitle) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(makeCardBackground());
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        card.setLayoutParams(lp);

        TextView tv = new TextView(this);
        tv.setText(title);
        tv.setTextColor(COL_TEXT);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        card.addView(tv);

        if (subtitle != null && !subtitle.isEmpty()) {
            TextView sub = new TextView(this);
            sub.setText(subtitle);
            sub.setTextColor(COL_MUTED);
            sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            slp.topMargin = dp(2);
            slp.bottomMargin = dp(10);
            sub.setLayoutParams(slp);
            card.addView(sub);
        }
        return card;
    }

    private LinearLayout makeHorizontalRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    private void addRowSpacing(View v, int rightMarginDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.rightMargin = dp(rightMarginDp);
        v.setLayoutParams(lp);
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(COL_BG);
        root.setPadding(dp(18), dp(12), dp(18), dp(12));

        // Top Header Bar
        LinearLayout header = makeHorizontalRow();
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hlp.bottomMargin = dp(10);
        header.setLayoutParams(hlp);

        LinearLayout titleCol = new LinearLayout(this);
        titleCol.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        titleCol.setLayoutParams(tlp);

        TextView appTitle = new TextView(this);
        appTitle.setText("SHIFT DRIVE  •  STANDALONE SETUP");
        appTitle.setTextColor(COL_TEXT);
        appTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f);
        appTitle.setTypeface(Typeface.DEFAULT_BOLD);
        titleCol.addView(appTitle);

        TextView padHint = new TextView(this);
        padHint.setText("Touch or Gamepad: D-Pad Move  •  A Select  •  B Cancel  •  Y Log  •  Start Play");
        padHint.setTextColor(COL_MUTED);
        padHint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        titleCol.addView(padHint);
        header.addView(titleCol);

        mStorageText = new TextView(this);
        mStorageText.setTextColor(COL_MUTED);
        mStorageText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        mStorageText.setPadding(dp(10), 0, dp(10), 0);
        header.addView(mStorageText);

        mHeaderBadge = new TextView(this);
        mHeaderBadge.setText("CHECKING...");
        mHeaderBadge.setTextColor(COL_TEXT);
        mHeaderBadge.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        mHeaderBadge.setTypeface(Typeface.DEFAULT_BOLD);
        mHeaderBadge.setPadding(dp(12), dp(6), dp(12), dp(6));
        GradientDrawable badgeBg = new GradientDrawable();
        badgeBg.setColor(COL_BTN);
        badgeBg.setCornerRadius(dp(14));
        mHeaderBadge.setBackground(badgeBg);
        header.addView(mHeaderBadge);

        root.addView(header);

        // Scrollable 5-Section Body
        ScrollView scroll = new ScrollView(this);
        LinearLayout.LayoutParams sclp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        scroll.setLayoutParams(sclp);

        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(body);

        // ---- Section 1: Game Disc (.iso or Folder) ----
        LinearLayout discCard = makeCard(
                "1. Game Disc (.iso or Extracted Folder)",
                "Pick your Forza Horizon 1 Xbox 360 .iso (streams directly via Android SAF) or an extracted media/ folder.");
        mDiscStatusText = new TextView(this);
        mDiscStatusText.setText("Checking game disc...");
        mDiscStatusText.setTextColor(COL_ACCENT);
        mDiscStatusText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        mDiscStatusText.setTypeface(Typeface.DEFAULT_BOLD);
        discCard.addView(mDiscStatusText);

        mDiscHintText = new TextView(this);
        mDiscHintText.setText("Requires ~10 GB free storage for extracted media + baked cache.");
        mDiscHintText.setTextColor(COL_MUTED);
        mDiscHintText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        LinearLayout.LayoutParams dhlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        dhlp.topMargin = dp(2);
        dhlp.bottomMargin = dp(10);
        mDiscHintText.setLayoutParams(dhlp);
        discCard.addView(mDiscHintText);

        LinearLayout discBtns = makeHorizontalRow();
        mBtnPickIso = makeButton("Pick .ISO File", COL_BTN, COL_BTN_FOCUS, true, v -> openIsoPicker());
        addRowSpacing(mBtnPickIso, 10);
        mBtnPickFolder = makeButton("Pick Extracted Folder", COL_BTN, COL_BTN_FOCUS, false, v -> openFolderPicker());
        discBtns.addView(mBtnPickIso);
        discBtns.addView(mBtnPickFolder);
        discCard.addView(discBtns);
        body.addView(discCard);

        // ---- Section 2: Updates & DLC (STFS) ----
        LinearLayout dlcCard = makeCard(
                "2. Updates & DLC (Xbox 360 STFS Packages)",
                "Optional: install Title Update v4, Horizon Rally, 1000 Club, and DLC car packs directly on-device.");
        mBtnAddDlc = makeButton("Add Updates / DLC (STFS)", COL_BTN, COL_BTN_FOCUS, false, v -> openStfsPicker());
        dlcCard.addView(mBtnAddDlc);

        mPackagesList = new LinearLayout(this);
        mPackagesList.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        plp.topMargin = dp(8);
        mPackagesList.setLayoutParams(plp);
        dlcCard.addView(mPackagesList);
        body.addView(dlcCard);

        // ---- Section 3: Mods ----
        LinearLayout modsCard = makeCard(
                "3. Mods (.zip Archives)",
                "Install mods into files/state/mods/, toggle enable/disable, and reorder load priority.");
        mBtnAddMod = makeButton("Install Mod (.zip)", COL_BTN, COL_BTN_FOCUS, false, v -> openModZipPicker());
        modsCard.addView(mBtnAddMod);

        mModsList = new LinearLayout(this);
        mModsList.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams mlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        mlp.topMargin = dp(8);
        mModsList.setLayoutParams(mlp);
        modsCard.addView(mModsList);
        body.addView(modsCard);

        // ---- Section 4: GPU Driver (Turnip) ----
        LinearLayout gpuCard = makeCard(
                "4. GPU Driver (Mesa Turnip for Adreno)",
                "Optional: import a custom Turnip .so or release .zip into files/state/drivers/turnip/.");
        mTurnipStatusText = new TextView(this);
        mTurnipStatusText.setText("Active Vulkan driver: System default");
        mTurnipStatusText.setTextColor(COL_MUTED);
        mTurnipStatusText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        LinearLayout.LayoutParams glp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        glp.bottomMargin = dp(8);
        mTurnipStatusText.setLayoutParams(glp);
        gpuCard.addView(mTurnipStatusText);

        mBtnImportTurnip = makeButton("Import Turnip (.so / .zip)", COL_BTN, COL_BTN_FOCUS, false,
                v -> openTurnipPicker());
        gpuCard.addView(mBtnImportTurnip);
        body.addView(gpuCard);

        // ---- Section 5: Build & Play ----
        LinearLayout buildCard = makeCard(
                "5. Build & Play (On-Device Baker)",
                "Fast Start bakes shared assets, Colorado world index, starter/campaign & traffic cars in minutes.");

        mFullRosterCheck = new CheckBox(this);
        mFullRosterCheck.setText("Bake Full Roster (all ~240 cars — slower, resumable)");
        mFullRosterCheck.setTextColor(COL_TEXT);
        mFullRosterCheck.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        mFullRosterCheck.setFocusable(true);
        mFullRosterCheck.setOnCheckedChangeListener((btn, checked) -> mFullRosterMode = checked);
        buildCard.addView(mFullRosterCheck);

        mStageLabel = new TextView(this);
        mStageLabel.setText("Idle");
        mStageLabel.setTextColor(COL_TEXT);
        mStageLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        mStageLabel.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams sllp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sllp.topMargin = dp(8);
        mStageLabel.setLayoutParams(sllp);
        buildCard.addView(mStageLabel);

        mStageDetail = new TextView(this);
        mStageDetail.setText("Ready when you are.");
        mStageDetail.setTextColor(COL_MUTED);
        mStageDetail.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        buildCard.addView(mStageDetail);

        mProgressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgressBar.setMax(1000);
        mProgressBar.setProgress(0);
        LinearLayout.LayoutParams pblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(10));
        pblp.topMargin = dp(6);
        pblp.bottomMargin = dp(12);
        mProgressBar.setLayoutParams(pblp);
        buildCard.addView(mProgressBar);

        LinearLayout actionRow = makeHorizontalRow();
        mBtnRunSetup = makeButton("Run Setup", COL_ACCENT_DARK, COL_ACCENT, true, v -> runSetupPipeline(mFullRosterMode));
        addRowSpacing(mBtnRunSetup, 10);

        mBtnBakeFull = makeButton("Bake Full Roster (240 cars)", COL_BTN, COL_BTN_FOCUS, false,
                v -> runSetupPipeline(true));
        addRowSpacing(mBtnBakeFull, 10);

        mBtnCancel = makeButton("Cancel", Color.parseColor("#7F1D1D"), COL_ERR, false, v -> cancelActiveWork());
        mBtnCancel.setEnabled(false);
        addRowSpacing(mBtnCancel, 10);

        mBtnToggleLog = makeButton("Show Log", COL_BTN, COL_BTN_FOCUS, false, v -> toggleLogVisibility());
        addRowSpacing(mBtnToggleLog, 10);

        mBtnPlay = makeButton("PLAY", COL_OK_DARK, COL_OK, true, v -> launchDrive());
        mBtnPlay.setEnabled(false);

        actionRow.addView(mBtnRunSetup);
        actionRow.addView(mBtnBakeFull);
        actionRow.addView(mBtnCancel);
        actionRow.addView(mBtnToggleLog);
        actionRow.addView(mBtnPlay);
        buildCard.addView(actionRow);

        // Collapsible Log Viewer
        mLogScroll = new ScrollView(this);
        mLogScroll.setBackgroundColor(COL_LOG_BG);
        mLogScroll.setPadding(dp(10), dp(8), dp(10), dp(8));
        mLogScroll.setVisibility(View.GONE);
        LinearLayout.LayoutParams lslp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(150));
        lslp.topMargin = dp(10);
        mLogScroll.setLayoutParams(lslp);

        mLogText = new TextView(this);
        mLogText.setTextColor(Color.parseColor("#CBD5E1"));
        mLogText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        mLogText.setTypeface(Typeface.MONOSPACE);
        mLogScroll.addView(mLogText);
        buildCard.addView(mLogScroll);

        body.addView(buildCard);
        root.addView(scroll);
        setContentView(root);
        mBtnPickIso.requestFocus();
    }

    private void toggleLogVisibility() {
        mLogVisible = !mLogVisible;
        mLogScroll.setVisibility(mLogVisible ? View.VISIBLE : View.GONE);
        mBtnToggleLog.setText(mLogVisible ? "Hide Log" : "Show Log");
        if (mLogVisible) {
            mLogScroll.post(() -> mLogScroll.fullScroll(View.FOCUS_DOWN));
        }
    }

    private void appendLog(String line) {
        if (line == null) return;
        runOnUiThread(() -> {
            if (mLogBuf.length() > 32768) {
                mLogBuf.delete(0, mLogBuf.length() - 16384);
            }
            mLogBuf.append(line).append('\n');
            if (mLogText != null) {
                mLogText.setText(mLogBuf.toString());
                if (mLogVisible) {
                    mLogScroll.post(() -> mLogScroll.fullScroll(View.FOCUS_DOWN));
                }
            }
        });
    }

    private void updateProgressUi(float fraction, String label, String detail) {
        int p = Math.max(0, Math.min(1000, Math.round(fraction * 1000f)));
        mProgressBar.setProgress(p);
        if (label != null) mStageLabel.setText(label);
        if (detail != null) mStageDetail.setText(detail);
    }

    private void setBusyState(boolean busy) {
        mBusy.set(busy);
        runOnUiThread(() -> {
            mBtnPickIso.setEnabled(!busy);
            mBtnPickFolder.setEnabled(!busy);
            mBtnAddDlc.setEnabled(!busy);
            mBtnAddMod.setEnabled(!busy);
            mBtnImportTurnip.setEnabled(!busy);
            mFullRosterCheck.setEnabled(!busy);
            mBtnRunSetup.setEnabled(!busy && (mHasGameDisc || mPendingIsoUri != null));
            mBtnBakeFull.setEnabled(!busy && mHasGameDisc);
            mBtnCancel.setEnabled(busy);
            mBtnPlay.setEnabled(!busy && mReadyToPlay);
        });
    }

    private void cancelActiveWork() {
        if (!mBusy.get()) return;
        mFolderCopyCancel.set(true);
        SetupBridge.nativeCancelSetup();
        appendLog("Cancellation requested...");
        updateProgressUi(mProgressBar.getProgress() / 1000f, "Cancelling...", "Waiting for active task to stop");
    }

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
            mDiscStatusText.setText("ISO selected: " + mPendingIsoName + " (tap Run Setup to extract & bake)");
            mDiscStatusText.setTextColor(COL_ACCENT);
            appendLog("Selected ISO: " + mPendingIsoName);
            setBusyState(false);
        } else if (requestCode == REQ_PICK_FOLDER) {
            Uri treeUri = data.getData();
            if (treeUri != null) {
                importExtractedFolderAsync(treeUri);
            }
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
        }
    }

    private void tryTakePersistablePermission(Uri uri) {
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Throwable ignored) {
        }
    }

    private void runSetupPipeline(boolean fullRoster) {
        if (!mBusy.compareAndSet(false, true)) return;
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                // Step A: Extract queued .ISO if user selected one
                if (mPendingIsoUri != null) {
                    appendLog("Extracting ISO: " + mPendingIsoName);
                    ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(mPendingIsoUri, "r");
                    if (pfd == null) {
                        appendLog("ERROR: Unable to open ISO file descriptor");
                        runOnUiThread(() -> updateProgressUi(0f, "Extraction failed", "Cannot open selected .ISO"));
                        return;
                    }
                    int fd = pfd.detachFd();
                    String resJson = SetupBridge.nativeExtractIsoFd(
                            fd, getGameDir().getAbsolutePath(), true, true, mBridgeListener);
                    JSONObject res = new JSONObject(resJson);
                    if (!res.optBoolean("ok", false)) {
                        String err = res.optString("error", "ISO extraction failed");
                        runOnUiThread(() -> updateProgressUi(0f, "ISO extraction stopped", err));
                        return;
                    }
                    mPendingIsoUri = null;
                    mPendingIsoName = null;
                }

                // Step B: Run in-process on-device baker
                appendLog("Starting on-device bake (" + (fullRoster ? "Full Roster" : "Fast Start") + ")...");
                String bakeJson = SetupBridge.nativeRunOnDeviceBake(
                        getGameDir().getAbsolutePath(),
                        getStateDir().getAbsolutePath(),
                        getPlayDir().getAbsolutePath(),
                        fullRoster,
                        true,
                        mBridgeListener);
                JSONObject bake = new JSONObject(bakeJson);
                if (bake.optBoolean("ok", false)) {
                    int total = bake.optInt("cars_baked", 0) + bake.optInt("cars_reused", 0);
                    runOnUiThread(() -> updateProgressUi(
                            1f, "Setup complete", total + " car(s) ready — tap PLAY to drive!"));
                } else {
                    String err = bake.optString("error", "Bake failed");
                    runOnUiThread(() -> updateProgressUi(0f, "Bake stopped", err));
                }
            } catch (Throwable t) {
                appendLog("ERROR: " + t.getMessage());
                runOnUiThread(() -> updateProgressUi(0f, "Setup error", String.valueOf(t.getMessage())));
            } finally {
                setBusyState(false);
                refreshLibraryAsync();
            }
        });
    }

    private void importExtractedFolderAsync(Uri treeUri) {
        if (!mBusy.compareAndSet(false, true)) return;
        mFolderCopyCancel.set(false);
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                appendLog("Importing extracted game folder via SAF...");
                String rootDocId = DocumentsContract.getTreeDocumentId(treeUri);
                String rootName = rootDocId;
                int slash = rootName.lastIndexOf('/');
                if (slash >= 0) rootName = rootName.substring(slash + 1);
                boolean isMediaRoot = "media".equalsIgnoreCase(rootName);
                File targetBase = isMediaRoot ? new File(getGameDir(), "media") : getGameDir();
                targetBase.mkdirs();

                int[] copied = new int[] { 0 };
                copyDocumentTreeRecursive(treeUri, rootDocId, targetBase, copied);
                if (mFolderCopyCancel.get()) {
                    appendLog("Folder import cancelled after " + copied[0] + " file(s).");
                } else {
                    appendLog("Imported " + copied[0] + " file(s) into " + targetBase.getAbsolutePath());
                    runOnUiThread(() -> updateProgressUi(1f, "Folder imported",
                            copied[0] + " files copied — tap Run Setup to bake"));
                }
            } catch (Throwable t) {
                appendLog("Folder import error: " + t.getMessage());
                runOnUiThread(() -> updateProgressUi(0f, "Folder import failed", String.valueOf(t.getMessage())));
            } finally {
                setBusyState(false);
                refreshLibraryAsync();
            }
        });
    }

    private void copyDocumentTreeRecursive(Uri treeUri, String parentDocId, File outDir, int[] copied) throws Exception {
        if (mFolderCopyCancel.get()) return;
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
                if (mFolderCopyCancel.get()) return;
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
                    runOnUiThread(() -> updateProgressUi(0.5f, "Importing game folder", name));
                    File tmp = new File(dest.getParentFile(), dest.getName() + ".part");
                    try (InputStream in = cr.openInputStream(docUri);
                         FileOutputStream out = new FileOutputStream(tmp)) {
                        if (in == null) continue;
                        byte[] buf = new byte[262144];
                        int n;
                        while ((n = in.read(buf)) > 0) {
                            if (mFolderCopyCancel.get()) {
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
        setBusyState(true);
        mExecutor.execute(() -> {
            int installed = 0;
            try {
                for (int i = 0; i < uris.size(); ++i) {
                    Uri u = uris.get(i);
                    String name = queryDisplayName(u);
                    appendLog("Installing STFS [" + (i + 1) + "/" + uris.size() + "]: " + name);
                    ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(u, "r");
                    if (pfd == null) {
                        appendLog("Skipped unreadable file: " + name);
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
                runOnUiThread(() -> updateProgressUi(1f, "Updates & DLC updated",
                        "Installed " + count + " package(s). Re-run Setup to bake new DLC cars."));
            } catch (Throwable t) {
                appendLog("STFS install error: " + t.getMessage());
            } finally {
                setBusyState(false);
                refreshLibraryAsync();
            }
        });
    }

    private void installModZipAsync(Uri uri) {
        if (!mBusy.compareAndSet(false, true)) return;
        setBusyState(true);
        mExecutor.execute(() -> {
            try {
                String name = queryDisplayName(uri);
                ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
                if (pfd == null) {
                    appendLog("Cannot open mod archive: " + name);
                    return;
                }
                int fd = pfd.detachFd();
                String json = SetupBridge.nativeInstallModZipFd(
                        fd, name, getModsDir().getAbsolutePath(), mBridgeListener);
                JSONObject obj = new JSONObject(json);
                if (obj.optBoolean("ok", false)) {
                    String modName = obj.optString("mod_name", name);
                    runOnUiThread(() -> updateProgressUi(1f, "Mod installed", modName));
                } else {
                    String err = obj.optString("error", "Mod install failed");
                    runOnUiThread(() -> updateProgressUi(0f, "Mod install failed", err));
                }
            } catch (Throwable t) {
                appendLog("Mod install error: " + t.getMessage());
            } finally {
                setBusyState(false);
                refreshLibraryAsync();
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
                    appendLog("Cannot open driver file: " + name);
                    return;
                }
                int fd = pfd.detachFd();
                String json = SetupBridge.nativeInstallTurnipFd(fd, name, getDriversDir().getAbsolutePath());
                JSONObject obj = new JSONObject(json);
                if (obj.optBoolean("ok", false)) {
                    appendLog("Installed Turnip driver: " + obj.optString("driver_name", name));
                    runOnUiThread(() -> updateProgressUi(1f, "GPU driver installed",
                            obj.optString("driver_name", name)));
                } else {
                    String err = obj.optString("error", "Driver import failed");
                    appendLog("Turnip import error: " + err);
                    runOnUiThread(() -> updateProgressUi(0f, "Driver import failed", err));
                }
            } catch (Throwable t) {
                appendLog("Turnip import error: " + t.getMessage());
            } finally {
                setBusyState(false);
                refreshLibraryAsync();
            }
        });
    }

    private void refreshLibraryAsync() {
        mExecutor.execute(() -> {
            try {
                String json = SetupBridge.nativeInspectLibrary(
                        getPlayDir().getAbsolutePath(), getStateDir().getAbsolutePath());
                JSONObject st = new JSONObject(json);
                runOnUiThread(() -> applyLibraryStatus(st));
            } catch (Throwable t) {
                appendLog("Inspect error: " + t.getMessage());
            }
        });
    }

    private String formatBytes(long bytes) {
        if (bytes <= 0) return "0 B";
        double gb = bytes / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1.0) return String.format(Locale.US, "%.1f GB free", gb);
        double mb = bytes / (1024.0 * 1024.0);
        return String.format(Locale.US, "%.0f MB free", mb);
    }

    private void applyLibraryStatus(JSONObject st) {
        mHasGameDisc = st.optBoolean("has_game_disc", false);
        mReadyToPlay = st.optBoolean("ready_to_play", false);
        int discCars = st.optInt("disc_car_count", 0);
        int bakedCars = st.optInt("baked_car_count", 0);
        boolean fullDone = st.optBoolean("full_roster_complete", false);
        boolean hasTurnip = st.optBoolean("has_turnip_driver", false);

        long freeBytes = st.optLong("free_storage_bytes", 0L);
        if (freeBytes <= 0L) {
            try {
                StatFs sf = new StatFs(getFilesRoot().getAbsolutePath());
                freeBytes = sf.getAvailableBytes();
            } catch (Throwable ignored) {
            }
        }
        mStorageText.setText(formatBytes(freeBytes));

        // Header readiness pill
        GradientDrawable badgeBg = new GradientDrawable();
        badgeBg.setCornerRadius(dp(14));
        if (mReadyToPlay) {
            mHeaderBadge.setText("READY (" + bakedCars + " CARS)");
            badgeBg.setColor(COL_OK_DARK);
        } else if (mHasGameDisc || mPendingIsoUri != null) {
            mHeaderBadge.setText("NEEDS BAKE");
            badgeBg.setColor(COL_ACCENT_DARK);
        } else {
            mHeaderBadge.setText("SETUP REQUIRED");
            badgeBg.setColor(COL_BTN);
        }
        mHeaderBadge.setBackground(badgeBg);

        // Section 1: Disc status
        if (mPendingIsoUri != null) {
            mDiscStatusText.setText("Queued ISO: " + mPendingIsoName + " (ready to extract & bake)");
            mDiscStatusText.setTextColor(COL_ACCENT);
        } else if (mHasGameDisc) {
            mDiscStatusText.setText("Verified Forza Horizon 1 disc (" + discCars + " car archives)");
            mDiscStatusText.setTextColor(COL_OK);
        } else {
            mDiscStatusText.setText("No game disc installed yet");
            mDiscStatusText.setTextColor(COL_ACCENT);
        }
        mDiscHintText.setText("Storage: " + formatBytes(freeBytes) + "  •  Path: " + getGameDir().getAbsolutePath());

        // Section 2: Installed Updates & DLC
        mPackagesList.removeAllViews();
        JSONArray pkgs = st.optJSONArray("packages");
        if (pkgs == null || pkgs.length() == 0) {
            TextView empty = new TextView(this);
            empty.setText("No updates or DLC packs installed.");
            empty.setTextColor(COL_MUTED);
            empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            mPackagesList.addView(empty);
        } else {
            for (int i = 0; i < pkgs.length(); ++i) {
                JSONObject p = pkgs.optJSONObject(i);
                if (p == null) continue;
                TextView row = new TextView(this);
                String tag = p.optString("kind", "dlc").toUpperCase(Locale.ROOT).replace('_', ' ');
                row.setText("[" + tag + "]  " + p.optString("name", p.optString("id", "")));
                row.setTextColor(COL_TEXT);
                row.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
                row.setPadding(dp(4), dp(3), dp(4), dp(3));
                mPackagesList.addView(row);
            }
        }

        // Section 3: Mods list with toggle + Up / Down reorder
        mModsList.removeAllViews();
        JSONArray mods = st.optJSONArray("mods");
        if (mods == null || mods.length() == 0) {
            TextView empty = new TextView(this);
            empty.setText("No mods installed.");
            empty.setTextColor(COL_MUTED);
            empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            mModsList.addView(empty);
        } else {
            for (int i = 0; i < mods.length(); ++i) {
                JSONObject m = mods.optJSONObject(i);
                if (m == null) continue;
                final String modName = m.optString("name", "");
                final String modTitle = m.optString("title", modName);
                final boolean enabled = m.optBoolean("enabled", true);
                final String summary = m.optString("summary", "");

                LinearLayout row = makeHorizontalRow();
                LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                rlp.topMargin = dp(4);
                row.setLayoutParams(rlp);

                CheckBox cb = new CheckBox(this);
                cb.setText(modTitle + (summary.isEmpty() ? "" : "  (" + summary + ")"));
                cb.setChecked(enabled);
                cb.setTextColor(COL_TEXT);
                cb.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
                cb.setFocusable(true);
                LinearLayout.LayoutParams cblp = new LinearLayout.LayoutParams(
                        0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
                cb.setLayoutParams(cblp);
                cb.setOnCheckedChangeListener((buttonView, isChecked) -> {
                    mExecutor.execute(() -> {
                        SetupBridge.nativeSetModEnabled(getModsDir().getAbsolutePath(), modName, isChecked);
                        refreshLibraryAsync();
                    });
                });

                Button up = makeButton("Up", COL_BTN, COL_BTN_FOCUS, false, v -> mExecutor.execute(() -> {
                    SetupBridge.nativeMoveMod(getModsDir().getAbsolutePath(), modName, -1);
                    refreshLibraryAsync();
                }));
                addRowSpacing(up, 6);

                Button down = makeButton("Down", COL_BTN, COL_BTN_FOCUS, false, v -> mExecutor.execute(() -> {
                    SetupBridge.nativeMoveMod(getModsDir().getAbsolutePath(), modName, 1);
                    refreshLibraryAsync();
                }));
                addRowSpacing(down, 6);

                Button del = makeButton("Remove", Color.parseColor("#451A1A"), COL_ERR, false,
                        v -> mExecutor.execute(() -> {
                            SetupBridge.nativeRemoveMod(getModsDir().getAbsolutePath(), modName);
                            refreshLibraryAsync();
                        }));

                row.addView(cb);
                row.addView(up);
                row.addView(down);
                row.addView(del);
                mModsList.addView(row);
            }
        }

        // Section 4: GPU driver status
        if (hasTurnip) {
            mTurnipStatusText.setText("Active Vulkan driver: Turnip custom driver installed");
            mTurnipStatusText.setTextColor(COL_OK);
        } else {
            mTurnipStatusText.setText("Active Vulkan driver: System default");
            mTurnipStatusText.setTextColor(COL_MUTED);
        }

        // Section 5: Buttons
        mBtnBakeFull.setText(fullDone ? "Full Roster Baked (" + bakedCars + " cars)" : "Bake Full Roster (240 cars)");
        if (!mBusy.get()) {
            mBtnRunSetup.setEnabled(mHasGameDisc || mPendingIsoUri != null);
            mBtnBakeFull.setEnabled(mHasGameDisc);
            mBtnPlay.setEnabled(mReadyToPlay);
        }
    }

    private void launchDrive() {
        Intent drive = new Intent(this, DriveActivity.class);
        drive.putExtra(DriveActivity.EXTRA_FORCE_PLAY, true);
        drive.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(drive);
        finish();
    }
}
