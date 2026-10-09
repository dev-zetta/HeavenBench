package app.zetta.heavenbench;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class LauncherActivity extends Activity {
    private static final int IMPORT_ASSETS = 1;
    private File storage;
    private TextView status;
    private Button importButton;
    private Button exploreButton;
    private Button benchmarkButton;
    private boolean busy;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (24 * getResources().getDisplayMetrics().density);
        content.setPadding(padding, padding, padding, padding);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        setContentView(scroll);

        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextSize(28);
        content.addView(title);
        TextView instructions = new TextView(this);
        instructions.setText(R.string.launcher_instructions);
        content.addView(instructions);
        status = new TextView(this);
        content.addView(status);
        importButton = button(content, R.string.import_assets, () -> {
            Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            picker.addCategory(Intent.CATEGORY_OPENABLE);
            picker.setType("*/*");
            picker.putExtra(Intent.EXTRA_MIME_TYPES,
                new String[] { "application/zip", "application/octet-stream", "application/x-zip-compressed" });
            startActivityForResult(picker, IMPORT_ASSETS);
        });
        exploreButton = button(content, R.string.explore, () -> launch(false));
        benchmarkButton = button(content, R.string.run_benchmark, () -> launch(true));
        try {
            storage = AppStorage.root(this);
            refresh();
        } catch (IOException error) {
            showError(error);
            setBusy(true);
        }
    }

    private Button button(LinearLayout parent, int label, Runnable action) {
        Button result = new Button(this);
        result.setText(label);
        result.setOnClickListener(view -> action.run());
        parent.addView(result);
        return result;
    }

    private void setBusy(boolean value) {
        busy = value;
        importButton.setEnabled(!value);
        boolean ready = !value && storage != null && new File(storage, "assets.zip").isFile();
        exploreButton.setEnabled(ready);
        benchmarkButton.setEnabled(ready);
    }

    private void refresh() {
        int ready = new File(storage, "assets.zip").isFile() ? R.string.assets_ready : R.string.assets_missing;
        status.setText(getString(R.string.storage_status, getString(ready), storage.getAbsolutePath()));
        setBusy(false);
    }

    private void showError(Exception error) {
        if (!isFinishing() && !isDestroyed()) {
            new AlertDialog.Builder(this).setTitle(R.string.app_name)
                .setMessage(error.getMessage()).setPositiveButton(android.R.string.ok, null).show();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != IMPORT_ASSETS || result != RESULT_OK || data == null || busy) return;
        Uri uri = data.getData();
        if (uri == null) return;
        setBusy(true);
        status.setText(R.string.importing_assets);
        new Thread(() -> {
            File pending = new File(storage, "assets-import.tmp");
            try {
                try (InputStream input = getContentResolver().openInputStream(uri);
                     FileOutputStream output = new FileOutputStream(pending)) {
                    if (input == null) throw new IOException("Cannot open selected archive.");
                    byte[] buffer = new byte[1024 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                    output.getFD().sync();
                }
                boolean hasWorld = false;
                try (ZipFile archive = new ZipFile(pending)) {
                    Enumeration<? extends ZipEntry> entries = archive.entries();
                    while (entries.hasMoreElements()) {
                        String name = entries.nextElement().getName();
                        if (name.equals("heaven/heaven.world") || name.endsWith("/heaven/heaven.world")) {
                            hasWorld = true;
                            break;
                        }
                    }
                }
                if (!hasWorld) throw new IOException("This archive has no Heaven scene. Select a ZIP created by the desktop asset packer.");
                File destination = new File(storage, "assets.zip");
                try {
                    Files.move(pending.toPath(), destination.toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                } catch (AtomicMoveNotSupportedException error) {
                    Files.move(pending.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING);
                }
                runOnUiThread(this::refresh);
            } catch (IOException | SecurityException error) {
                pending.delete();
                runOnUiThread(() -> { refresh(); showError(error); });
            }
        }, "asset-import").start();
    }

    private void launch(boolean benchmark) {
        setBusy(true);
        status.setText(R.string.preparing_renderer);
        new Thread(() -> {
            try {
                AppStorage.prepare(this, storage);
                Intent renderer = new Intent(this, HeavenActivity.class);
                renderer.putExtra("storage", storage.getAbsolutePath());
                renderer.putExtra("benchmark", benchmark);
                MicroprofileLaunch.forward(getIntent(), renderer);
                runOnUiThread(() -> { refresh(); startActivity(renderer); });
            } catch (IOException | IllegalArgumentException error) {
                runOnUiThread(() -> { refresh(); showError(error); });
            }
        }, "renderer-start").start();
    }
}
