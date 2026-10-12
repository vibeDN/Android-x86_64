// SPDX-License-Identifier: GPL-2.0-or-later
package ours.test;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/** Calls every native of Bridge and shows/logs PASS/FAIL lines (tag armtr-test). */
public class MainActivity extends Activity {
  private final StringBuilder out = new StringBuilder();
  private int fails;

  private void check(String name, boolean ok, Object got) {
    String l = (ok ? "PASS " : "FAIL ") + name + " = " + got;
    if (!ok) fails++;
    Log.i("armtr-test", l);
    out.append(l).append('\n');
  }

  @Override protected void onCreate(Bundle b) {
    super.onCreate(b);
    try {
      System.loadLibrary("jni_bridge_test");
      check("add(5,7)", Bridge.add(5, 7) == 22, Bridge.add(5, 7));
      double want = 1 + 2.5 + 0.25 + 4.0 + 1.5 + 2 * 2.5 + 3 * 3.5 + 4 * 4.5 + 5 * 5.5 + 6 * 6.5 + 7 * 7.5 + 8 * 8.5
          + 2 * 10.0 + 3 * 100.0 + 4 * 1000.0 + 5 * 1e4 + 6 * 1e5 + 7 * 1e6;
      double m = Bridge.mix(1, 2.5, 0.25f, 4, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 2, 3, 4, 5, 6, 7);
      check("mix", Math.abs(m - want) < 1e-6, m);
      check("half(3)", Bridge.half(3f) == 1.5f, Bridge.half(3f));
      check("isNeg(-4)", Bridge.isNeg(-4) && !Bridge.isNeg(4), Bridge.isNeg(-4));
      check("neg16(5)", Bridge.neg16((short) 5) == -5, Bridge.neg16((short) 5));
      check("callback(5)", Bridge.callback(5) == 111, Bridge.callback(5));
      check("hello()", "hello from arm64".equals(Bridge.hello()), Bridge.hello());
      check("thread()", Bridge.thread() == 0x10006, Integer.toHexString(Bridge.thread()));
      int t1 = Bridge.tls();
      final int[] t2 = new int[1];
      Thread th = new Thread(() -> t2[0] = Bridge.tls());
      th.start();
      th.join();
      check("tls main/other thread", t1 >= 1 && t2[0] == 1, t1 + "/" + t2[0]);
      check("log(42)", Bridge.log(42) > 0, "see logcat tag armtr-test: n=42 ok 0.50");
    } catch (Throwable t) {
      fails++;
      Log.e("armtr-test", "exception", t);
      out.append("EXCEPTION ").append(t).append('\n');
    }
    out.append(fails == 0 ? "ALL PASS" : fails + " FAILED");
    Log.i("armtr-test", fails == 0 ? "ALL PASS" : fails + " FAILED");
    TextView v = new TextView(this);
    v.setTextSize(20);
    v.setText("armtr arm64 JNI test (" + System.getProperty("os.arch") + ")\n" + out);
    setContentView(v);
  }
}
