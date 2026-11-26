// Shared Android settings.txt persistence, profile save path lookup, and
// cross-fade activity transition utilities shared by SetupActivity and DriveActivity.
package net.shiftengine.drive;

import android.app.Activity;
import android.os.Build;
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
