// The Android app's activity (M7): SDL's activity with SDL linked
// statically into libmain.so, so only that library is loaded. The drive
// starts with no arguments and finds its play folder in the app's external
// files folder (Android/data/net.shiftengine.drive/files/play).
package net.shiftengine.drive;

import org.libsdl.app.SDLActivity;

public class DriveActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
