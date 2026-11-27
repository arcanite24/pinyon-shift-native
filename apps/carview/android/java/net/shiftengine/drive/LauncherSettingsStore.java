// Shared Android settings.txt persistence, profile save path lookup, and
// cross-fade activity transition utilities shared by SetupActivity and DriveActivity.
package net.shiftengine.drive;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Environment;
import android.os.StatFs;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class LauncherSettingsStore {
    public static final String KEY_PRESET = "preset";
    public static final String KEY_TOUCH_CONTROLS = "touch-controls";
    public static final String KEY_TOUCH_STEER = "touch-steer";
    public static final String KEY_FPS_CAP = "fps-cap";
    public static final String KEY_VSYNC = "vsync";
    public static final String KEY_GPU_DRIVER = "android-gpu-driver";

    public static final String[] PRESET_LABELS = { "Smooth 60", "Quality 30", "Low Power", "Low", "High" };
    public static final String[] PRESET_VALUES = { "smooth-60", "quality-30", "low-power", "low", "high" };

    public static final String[] TOUCH_LABELS = { "Auto", "On", "Off" };
    public static final String[] TOUCH_VALUES = { "auto", "on", "off" };

    public static final String[] STEER_LABELS = { "Buttons", "Tilt", "Wheel" };
    public static final String[] STEER_VALUES = { "buttons", "tilt", "stick" };

    public static final String[] FPS_LABELS = { "60", "30", "40", "Display" };
    public static final String[] FPS_VALUES = { "60", "30", "40", "display" };

    public static final String[] VSYNC_LABELS = { "FIFO", "Mailbox", "Immediate" };
    public static final String[] VSYNC_VALUES = { "fifo", "mailbox", "immediate" };

    public static final String[] DRIVER_LABELS = { "Auto", "Turnip", "System" };
    public static final String[] DRIVER_VALUES = { "auto", "turnip", "system" };

    private static final String PREFS_NAME = "shift_launcher_prefs";
    private static final String PREF_PLAY_STORAGE_ROOT = "play_storage_root";
    private static final long MIN_SD_PARTITION_BYTES = 1024L * 1024L * 1024L; // Ignore < 1 GiB boot partitions (e.g. FAT boot)

    public static final class StorageOption {
        public final File rootDir;
        public final String label;
        public final String shortName;
        public final boolean isSdCard;
        public final long totalBytes;
        public final long freeBytes;
        public final boolean hasPlayData;

        public StorageOption(File rootDir, String label, String shortName, boolean isSdCard,
                             long totalBytes, long freeBytes, boolean hasPlayData) {
            this.rootDir = rootDir;
            this.label = label;
            this.shortName = shortName;
            this.isSdCard = isSdCard;
            this.totalBytes = totalBytes;
            this.freeBytes = freeBytes;
            this.hasPlayData = hasPlayData;
        }
    }

    public static String formatCompactBytes(long bytes) {
        if (bytes <= 0L) return "0 B";
        double gb = bytes / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 10.0) return String.format(Locale.US, "%.0f GB", gb);
        if (gb >= 1.0) return String.format(Locale.US, "%.1f GB", gb);
        double mb = bytes / (1024.0 * 1024.0);
        return String.format(Locale.US, "%.0f MB", mb);
    }

    public static File getInternalFilesRoot(Context ctx) {
        if (ctx == null) return null;
        File ext = ctx.getExternalFilesDir(null);
        return ext != null ? ext : ctx.getFilesDir();
    }

    // Enumerates usable app-owned storage roots across Internal storage and mounted
    // SD card partitions (skipping tiny <1 GiB boot partitions unless they already hold play data).
    public static List<StorageOption> listPlayStorageOptions(Context ctx) {
        List<StorageOption> out = new ArrayList<>();
        if (ctx == null) return out;
        File[] dirs = ctx.getExternalFilesDirs(null);
        File internal = getInternalFilesRoot(ctx);
        if (dirs == null || dirs.length == 0) {
            if (internal != null) dirs = new File[] { internal };
            else return out;
        }
        int sdCount = 0;
        List<File> validSd = new ArrayList<>();
        for (int i = 1; i < dirs.length; ++i) {
            File d = dirs[i];
            if (d == null) continue;
            try {
                if (!Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState(d))) continue;
                if (!d.exists()) d.mkdirs();
                if (!d.isDirectory()) continue;
                StatFs sf = new StatFs(d.getAbsolutePath());
                long total = sf.getTotalBytes();
                boolean hasPlay = new File(d, "play/play.txt").isFile();
                if (total < MIN_SD_PARTITION_BYTES && !hasPlay) continue;
                validSd.add(d);
            } catch (Throwable ignored) {
            }
        }
        sdCount = validSd.size();

        if (dirs[0] != null) {
            File d0 = dirs[0];
            if (!d0.exists()) d0.mkdirs();
            long total = 0L, free = 0L;
            try {
                StatFs sf = new StatFs(d0.getAbsolutePath());
                total = sf.getTotalBytes();
                free = sf.getAvailableBytes();
            } catch (Throwable ignored) {
            }
            boolean hasPlay = new File(d0, "play/play.txt").isFile();
            out.add(new StorageOption(
                    d0,
                    "Internal (" + formatCompactBytes(free) + ")",
                    "Internal",
                    false, total, free, hasPlay));
        }

        for (File sd : validSd) {
            long total = 0L, free = 0L;
            try {
                StatFs sf = new StatFs(sd.getAbsolutePath());
                total = sf.getTotalBytes();
                free = sf.getAvailableBytes();
            } catch (Throwable ignored) {
            }
            boolean hasPlay = new File(sd, "play/play.txt").isFile();
            String volTag = extractVolumeTag(sd.getAbsolutePath());
            String shortName = (sdCount > 1 && !volTag.isEmpty()) ? ("SD " + volTag) : "SD Card";
            out.add(new StorageOption(
                    sd,
                    shortName + " (" + formatCompactBytes(free) + ")",
                    shortName,
                    true, total, free, hasPlay));
        }
        return out;
    }

    private static String extractVolumeTag(String absPath) {
        if (absPath == null) return "";
        String prefix = "/storage/";
        if (absPath.startsWith(prefix)) {
            int slash = absPath.indexOf('/', prefix.length());
            if (slash > prefix.length()) return absPath.substring(prefix.length(), slash);
        }
        return "";
    }

    public static File resolvePlayStorageRoot(Context ctx) {
        File fallback = getInternalFilesRoot(ctx);
        if (ctx == null) return fallback;
        try {
            SharedPreferences prefs = ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
            String saved = prefs.getString(PREF_PLAY_STORAGE_ROOT, "");
            if (saved != null && !saved.isEmpty()) {
                for (StorageOption opt : listPlayStorageOptions(ctx)) {
                    if (opt.rootDir.getAbsolutePath().equals(saved)) {
                        return opt.rootDir;
                    }
                }
            }
        } catch (Throwable ignored) {
        }
        return fallback;
    }

    public static void setPlayStorageRoot(Context ctx, File rootDir) {
        if (ctx == null || rootDir == null) return;
        try {
            SharedPreferences prefs = ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
            prefs.edit().putString(PREF_PLAY_STORAGE_ROOT, rootDir.getAbsolutePath()).apply();
        } catch (Throwable ignored) {
        }
    }

    private LauncherSettingsStore() {}

    public static File settingsFile(File filesRoot) {
        return new File(filesRoot, "settings.txt");
    }

    public static Map<String, String> readAll(File settingsFile) {
        Map<String, String> map = new LinkedHashMap<>();
        if (settingsFile == null || !settingsFile.isFile()) return map;
        try (BufferedReader br = new BufferedReader(
                new InputStreamReader(new FileInputStream(settingsFile), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                String trimmed = line.trim();
                if (trimmed.isEmpty() || trimmed.startsWith("#")) continue;
                if (trimmed.startsWith("--")) trimmed = trimmed.substring(2);
                int sp = trimmed.indexOf(' ');
                if (sp < 0) {
                    map.put(trimmed, "");
                } else {
                    String k = trimmed.substring(0, sp).trim();
                    String v = trimmed.substring(sp + 1).trim();
                    if (!k.isEmpty()) map.put(k, v);
                }
            }
        } catch (Throwable ignored) {
        }
        return map;
    }

    public static void writeAll(File settingsFile, Map<String, String> values) {
        if (settingsFile == null || values == null) return;
        File parent = settingsFile.getParentFile();
        if (parent != null && !parent.exists()) parent.mkdirs();
        File tmp = new File(settingsFile.getParentFile(), settingsFile.getName() + ".tmp");
        try (OutputStreamWriter out = new OutputStreamWriter(
                new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
            out.write("# shift-engine settings: managed by Shift Engine Android Launcher\n");
            for (Map.Entry<String, String> e : values.entrySet()) {
                String k = e.getKey();
                if (k == null || k.trim().isEmpty()) continue;
                String v = e.getValue();
                if (v == null || v.isEmpty()) {
                    out.write(k.trim() + "\n");
                } else {
                    out.write(k.trim() + " " + v.trim() + "\n");
                }
            }
            out.flush();
        } catch (Throwable ignored) {
            tmp.delete();
            return;
        }
        if (settingsFile.exists()) settingsFile.delete();
        tmp.renameTo(settingsFile);
    }

    public static String getOrDefault(Map<String, String> map, String key, String defVal) {
        if (map == null) return defVal;
        String v = map.get(key);
        return (v != null && !v.isEmpty()) ? v.toLowerCase(Locale.ROOT) : defVal;
    }

    public static int indexOfValue(String[] values, String target, int fallback) {
        if (target != null) {
            String t = target.toLowerCase(Locale.ROOT).trim();
            if ("wheel".equals(t) || "thumbstick".equals(t)) t = "stick";
            if ("always".equals(t)) t = "on";
            if ("0".equals(t) && values == FPS_VALUES) t = "display";
            for (int i = 0; i < values.length; ++i) {
                if (values[i].equalsIgnoreCase(t)) return i;
            }
        }
        return fallback;
    }

    // Resolves the active profile's save.txt under filesRoot (profiles.txt ->
    // profiles/<name>/save.txt, or default drive/save.txt).
    public static File resolveActiveSaveFile(File filesRoot) {
        File defaultSave = new File(new File(filesRoot, "drive"), "save.txt");
        File profilesTxt = new File(filesRoot, "profiles.txt");
        if (!profilesTxt.isFile()) return defaultSave;
        String active = "default";
        try (BufferedReader br = new BufferedReader(
                new InputStreamReader(new FileInputStream(profilesTxt), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                String t = line.trim();
                if (t.startsWith("active ")) {
                    String a = t.substring(7).trim();
                    if (!a.isEmpty()) active = a;
                    break;
                }
            }
        } catch (Throwable ignored) {
        }
        if ("default".equals(active)) return defaultSave;
        return new File(new File(new File(filesRoot, "profiles"), active), "save.txt");
    }

    // Smooth cross-fade activity transition (API 34+ overrideActivityTransition
    // with automatic fallback to overridePendingTransition on API 29-33).
    @SuppressWarnings("deprecation")
    public static void applyCrossFadeTransition(Activity activity, boolean closing) {
        if (activity == null) return;
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                int type = closing
                        ? Activity.OVERRIDE_TRANSITION_CLOSE
                        : Activity.OVERRIDE_TRANSITION_OPEN;
                activity.overrideActivityTransition(
                        type, android.R.anim.fade_in, android.R.anim.fade_out);
            } else {
                activity.overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
            }
        } catch (Throwable ignored) {
            try {
                activity.overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
            } catch (Throwable ignored2) {
            }
        }
    }

    // Lightweight Java fallback parser for drive/save.txt if JNI inspection is unavailable.
    public static final class FallbackSaveInfo {
        public boolean hasSave = false;
        public String car = "";
        public long credits = 0L;
        public int garageCars = 0;
        public int signsFound = 0;
        public int level = 0;
    }

    public static FallbackSaveInfo parseSaveFileFallback(File saveTxt) {
        FallbackSaveInfo info = new FallbackSaveInfo();
        if (saveTxt == null || !saveTxt.isFile()) return info;
        try (BufferedReader br = new BufferedReader(
                new InputStreamReader(new FileInputStream(saveTxt), StandardCharsets.UTF_8))) {
            String line;
            List<String> lines = new ArrayList<>();
            while ((line = br.readLine()) != null) lines.add(line.trim());
            if (lines.isEmpty() || !lines.get(0).startsWith("shift-drive-save")) return info;
            info.hasSave = true;
            for (String l : lines) {
                if (l.startsWith("car ")) {
                    info.car = l.substring(4).trim();
                } else if (l.startsWith("credits ")) {
                    String[] parts = l.substring(8).trim().split("\\s+");
                    if (parts.length >= 1) info.credits = Long.parseLong(parts[0]);
                } else if (l.startsWith("garage ")) {
                    String rest = l.substring(7).trim();
                    if (!rest.isEmpty()) info.garageCars = rest.split("\\s+").length;
                } else if (l.startsWith("signs ")) {
                    String rest = l.substring(6).trim();
                    if (!rest.isEmpty()) info.signsFound = rest.split("\\s+").length;
                } else if (l.startsWith("level ")) {
                    info.level = Integer.parseInt(l.substring(6).trim());
                }
            }
        } catch (Throwable ignored) {
        }
        return info;
    }
}
