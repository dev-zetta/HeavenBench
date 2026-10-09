package app.zetta.heavenbench;

import android.content.Context;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

final class AppStorage {
    private AppStorage() {}

    static File root(Context context) throws IOException {
        File directory = context.getExternalFilesDir(null);
        if (directory == null) directory = context.getFilesDir();
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Cannot create app storage: " + directory);
        }
        return directory;
    }

    static void prepare(Context context, File root) throws IOException {
        File config = new File(root, "config.json");
        if (!config.exists()) {
            try (FileOutputStream output = new FileOutputStream(config)) {
                output.write(("{\"width\":1280,\"height\":720,"
                    + "\"quality_preset\":\"balanced\",\"texture_quality\":\"original\"}\n")
                    .getBytes(StandardCharsets.UTF_8));
            }
        }
        File resources = new File(root, "resources");
        // These are project-owned fallback resources, never the proprietary
        // Heaven asset archive. Refresh resources when starting a renderer.
        copyAssets(context, "resources", resources);
    }

    private static void copyAssets(Context context, String source, File target)
            throws IOException {
        String[] children = context.getAssets().list(source);
        if (children != null && children.length > 0) {
            if (!target.isDirectory() && !target.mkdirs()) {
                throw new IOException("Cannot create resource directory: " + target);
            }
            for (String child : children) {
                copyAssets(context, source + "/" + child, new File(target, child));
            }
        } else {
            try (InputStream input = context.getAssets().open(source);
                 FileOutputStream output = new FileOutputStream(target)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            }
        }
    }
}
