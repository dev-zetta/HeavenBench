package app.zetta.heavenbench;

import android.content.Intent;
import java.io.File;
import java.util.ArrayList;

/** Optional launcher-to-native trace arguments. Numeric validation belongs to the native CLI. */
final class MicroprofileLaunch {
    private MicroprofileLaunch() {}

    private static void validateBase(String base) {
        if (base != null && !base.matches("[A-Za-z0-9][A-Za-z0-9._-]{0,79}")) {
            throw new IllegalArgumentException(
                "microprofile must be a basename of 1-80 letters, digits, dots, underscores or hyphens, starting with a letter or digit.");
        }
    }

    static void forward(Intent source, Intent destination) {
        String base = source.getStringExtra("microprofile");
        validateBase(base);
        if (base != null) destination.putExtra("microprofile", base);
        String seconds = source.getStringExtra("microprofile_seconds");
        if (seconds != null) destination.putExtra("microprofile_seconds", seconds);
        if (source.getBooleanExtra("microprofile_visible", false)) {
            destination.putExtra("microprofile_visible", true);
        }
    }

    static void append(ArrayList<String> arguments, Intent intent, File storage) {
        String base = intent.getStringExtra("microprofile");
        validateBase(base);
        if (base != null) {
            arguments.add("--microprofile");
            arguments.add(new File(new File(storage, "profiles"), base).getAbsolutePath());
        }
        String seconds = intent.getStringExtra("microprofile_seconds");
        if (seconds != null) {
            arguments.add("--microprofile-seconds");
            arguments.add(seconds);
        }
        if (intent.getBooleanExtra("microprofile_visible", false)) arguments.add("--microprofile-visible");
    }
}
