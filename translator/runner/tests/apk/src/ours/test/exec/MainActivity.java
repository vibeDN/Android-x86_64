// SPDX-License-Identifier: GPL-2.0-or-later
// Runs the arm64 executable lib/arm64-v8a/libexec_hello.so like Tor/Syncthing apps do (ProcessBuilder, sh -c),
// and once with "exec" (the guest execs itself). Logs "armtr-exec" PASS/FAIL lines.
package ours.test.exec;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;

public class MainActivity extends Activity {
    static final String T = "armtr-exec";

    static String run(String... cmd) throws Exception {
        Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        InputStream in = p.getInputStream();
        byte[] b = new byte[4096];
        for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
        return "rc=" + p.waitFor() + " " + o.toString().trim();
    }

    static void check(String name, String out, String want) {
        Log.i(T, (out.startsWith("rc=0") && out.contains(want) ? "PASS " : "FAIL ") + name + ": " + out);
    }

    @Override
    protected void onCreate(Bundle s) {
        super.onCreate(s);
        final String exe = getApplicationInfo().nativeLibraryDir + "/libexec_hello.so";
        new Thread(() -> {
            try {
                check("processbuilder", run(exe, "a"), "argc=2 argv0=" + exe + " argv1=a");
                check("sh -c", run("/system/bin/sh", "-c", exe + " b"), "argv1=b");
                check("guest execve", run(exe, "exec"), "argv0=exec_hello argv1=child");
            } catch (Exception e) {
                Log.i(T, "FAIL exception " + e);
            }
            Log.i(T, "done");
        }).start();
    }
}
