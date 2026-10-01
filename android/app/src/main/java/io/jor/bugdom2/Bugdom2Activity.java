package io.jor.bugdom2;

import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class Bugdom2Activity extends SDLActivity {
    private static final String TAG = "Bugdom2";

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL3", "Bugdom2" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        extractGameDataIfNeeded();
        super.onCreate(savedInstanceState);
    }

    // The game reads its data files through regular file paths,
    // so copy them out of the APK into internal storage once per install/update.
    private void extractGameDataIfNeeded() {
        File dataDir = new File(getFilesDir(), "Data");
        File stamp = new File(getFilesDir(), "Data.stamp");
        String expected;
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            expected = pi.versionCode + ":" + pi.lastUpdateTime;
        } catch (Exception e) {
            expected = "unknown";
        }

        if (dataDir.isDirectory() && stamp.isFile() && expected.equals(readText(stamp))) {
            return;
        }

        Log.i(TAG, "Extracting game data to " + dataDir);
        deleteRecursive(dataDir);
        try {
            copyAssetDir(getAssets(), "Data", dataDir);
            writeText(stamp, expected);
        } catch (IOException e) {
            Log.e(TAG, "Failed to extract game data", e);
        }
    }

    private static void copyAssetDir(AssetManager am, String assetPath, File dest) throws IOException {
        String[] children = am.list(assetPath);
        if (children == null || children.length == 0) {
            // It's a file
            dest.getParentFile().mkdirs();
            try (InputStream in = am.open(assetPath); OutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
            return;
        }
        dest.mkdirs();
        for (String child : children) {
            copyAssetDir(am, assetPath + "/" + child, new File(dest, child));
        }
    }

    private static void deleteRecursive(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) {
                deleteRecursive(k);
            }
        }
        f.delete();
    }

    private static String readText(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] b = new byte[(int) f.length()];
            int n = in.read(b);
            return new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String s) throws IOException {
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(s.getBytes("UTF-8"));
        }
    }
}
