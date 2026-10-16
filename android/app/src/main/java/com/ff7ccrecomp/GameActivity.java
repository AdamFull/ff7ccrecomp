package com.ff7ccrecomp;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Toast;

import org.json.JSONObject;
import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Hosts the game: SDL loads libmain.so (SDL3 is linked statically into it) and calls SDL_main. */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    /** Optional command-line options for testing: adb shell am start -n com.ff7ccrecomp/.GameActivity -e args "--perf" */
    @Override
    protected String[] getArguments() {
        final String args = getIntent() != null ? getIntent().getStringExtra("args") : null;
        if (args == null || args.trim().isEmpty()) return new String[0];
        return args.trim().split("\\s+");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        importGpuDrivers();
        // Use the whole panel, including the area beside a camera cutout.
        getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
    }

    /** Called from native code: the panel refresh rate to prefer (0: the device's default). */
    public void setPreferredRefreshRate(final float hz) {
        runOnUiThread(() -> {
            final WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.preferredRefreshRate = hz;
            getWindow().setAttributes(attributes);
            hideSystemBars();  // new window attributes bring the system bars back
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    // ---- custom GPU drivers (psprecore runtime/src/platform/gpu_driver.hpp) -------------------------------------

    private static final int PICK_GPU_DRIVER = 0x6770;

    /** Called from native code: where the APK's native libraries are (libadrenotools loads its hooks from there). */
    public String nativeLibraryDir() {
        return getApplicationInfo().nativeLibraryDir;
    }

    /** Called from native code: lets the player pick a driver package (.zip), which is then installed. */
    public void pickGpuDriver() {
        runOnUiThread(() -> {
            final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES,
                    new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            try {
                startActivityForResult(intent, PICK_GPU_DRIVER);
            } catch (Exception e) {
                Toast.makeText(this, "No file picker: " + e.getMessage(), Toast.LENGTH_LONG).show();
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_GPU_DRIVER) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        new Thread(() -> {
            String message;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                message = "GPU driver installed: " + installGpuDriver(in, uri.getLastPathSegment()) +
                          ". Choose it in the menu; it applies when the game starts again.";
            } catch (Exception e) {
                message = "Not a GPU driver package: " + e.getMessage();
            }
            final String shown = message;
            runOnUiThread(() -> Toast.makeText(this, shown, Toast.LENGTH_LONG).show());
        }, "gpu-driver-install").start();
    }

    private File driversDir() {
        return new File(getFilesDir(), "gpu_drivers");
    }

    /**
     * Zips put into Android/data/<package>/files/gpu_drivers (scripts/build-android.ps1 -Driver) are installed once
     * each; the zips stay where they are.
     */
    private void importGpuDrivers() {
        final File inbox = getExternalFilesDir("gpu_drivers");
        final File[] zips = inbox != null ? inbox.listFiles((dir, name) -> name.toLowerCase().endsWith(".zip")) : null;
        if (zips == null || zips.length == 0) return;
        final File record = new File(driversDir(), ".imported");
        final Set<String> imported = new HashSet<>();
        try {
            if (record.exists()) imported.addAll(Files.readAllLines(record.toPath(), StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
        boolean changed = false;
        for (File zip : zips) {
            final String key = zip.getName() + "	" + zip.length() + "	" + zip.lastModified();
            if (imported.contains(key)) continue;
            try (InputStream in = new FileInputStream(zip)) {
                installGpuDriver(in, zip.getName());
                imported.add(key);
                changed = true;
            } catch (Exception e) {
                android.util.Log.w("ff7cc", "GPU driver " + zip + ": " + e.getMessage());
            }
        }
        if (changed) {
            try {
                Files.write(record.toPath(), imported, StandardCharsets.UTF_8);
            } catch (IOException ignored) {
            }
        }
    }

    /**
     * Unpacks a driver package (meta.json naming the library, and the library; folders inside the zip are ignored)
     * into gpu_drivers/<its name>, replacing an earlier install of it. Returns the driver's name.
     */
    private String installGpuDriver(InputStream in, String fallbackName) throws IOException {
        final File dir = driversDir();
        final File incoming = new File(dir, ".incoming");
        deleteTree(incoming);
        if (!incoming.mkdirs()) throw new IOException("cannot create " + incoming);
        try (ZipInputStream zip = new ZipInputStream(in)) {
            final byte[] buffer = new byte[1 << 16];
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                if (entry.isDirectory()) continue;
                final String name = new File(entry.getName()).getName();  // flat: no paths from the zip
                if (name.isEmpty() || name.startsWith(".")) continue;
                try (OutputStream out = new FileOutputStream(new File(incoming, name))) {
                    for (int n; (n = zip.read(buffer)) > 0;) out.write(buffer, 0, n);
                }
            }
        }
        final File meta = new File(incoming, "meta.json");
        if (!meta.exists()) throw new IOException("no meta.json in the package");
        final JSONObject json;
        try {
            json = new JSONObject(new String(Files.readAllBytes(meta.toPath()), StandardCharsets.UTF_8));
        } catch (Exception e) {
            throw new IOException("meta.json: " + e.getMessage());
        }
        final String library = json.optString("libraryName", "");
        if (library.isEmpty() || !new File(incoming, library).exists())
            throw new IOException("the library meta.json names (" + library + ") is not in the package");
        String name = json.optString("name", "");
        if (name.isEmpty()) name = fallbackName != null ? fallbackName.replaceAll("(?i)\\.zip$", "") : "driver";
        final String id = name.replaceAll("[^A-Za-z0-9._-]+", "_").replaceAll("^[._]+", "");
        final File target = new File(dir, id.isEmpty() ? "driver" : id);
        deleteTree(target);
        if (!incoming.renameTo(target)) throw new IOException("cannot move the driver to " + target);
        return name;
    }

    private static void deleteTree(File file) {
        final File[] children = file.listFiles();
        if (children != null)
            for (File child : children) deleteTree(child);
        file.delete();
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            // Android 10: no WindowInsetsController yet, the system UI flags instead (and a fullscreen window: SDL
            // marks it FORCE_NOT_FULLSCREEN, which keeps the status bar whatever the flags say).
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN |
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            return;
        }
        final WindowInsetsController controller = getWindow().getInsetsController();
        if (controller == null) return;
        controller.hide(WindowInsets.Type.systemBars());
        controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }
}
