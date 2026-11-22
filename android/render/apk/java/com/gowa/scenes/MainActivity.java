package com.gowa.scenes;
import android.app.Activity; import android.os.Bundle; import android.content.Intent;
import java.io.File; import java.io.FileWriter;
/** am start -n com.gowa.scenes/.MainActivity --es args "a:--shader" --es tag name [--es env "K=V,K2=V2"] (comma-separated: adb shell re-splits on spaces) ; log in files/<tag>.log, exit code in files/<tag>.rc */
public class MainActivity extends Activity {
  @Override protected void onCreate(Bundle b) {
    super.onCreate(b);
    final Intent i = getIntent(); String ra = i.getStringExtra("args"); if (ra == null) ra = ""; if (ra.startsWith("a:")) ra = ra.substring(2);
    final String a = ra.replace(',', ' ');   // "a:--draw,--frames=60": adb shell drops empty args and am parses leading "--"
    final String tag = i.getStringExtra("tag") == null ? "run" : i.getStringExtra("tag");
    final String env = i.getStringExtra("env");
    final File dir = getFilesDir();
    new Thread(new Runnable() { public void run() {
      int rc = -1;
      try {
        if (env != null) for (String kv : env.split(",")) { int e = kv.indexOf('='); if (e > 0) Runner.setenv(kv.substring(0, e), kv.substring(e + 1)); }
        rc = a.equals("repro") ? Repro.run(new File(dir, tag + ".log").getPath()) : Runner.run(new File(dir, tag + ".log").getPath(), a);
      } catch (Throwable t) { rc = -2; }
      try (FileWriter w = new FileWriter(new File(dir, tag + ".rc"))) { w.write(Integer.toString(rc)); } catch (Exception e) {}
      runOnUiThread(new Runnable() { public void run() { finish(); } });
    } }).start();
  }
}
