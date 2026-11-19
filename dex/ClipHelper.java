// Clipboard bridge loaded by app_process as the shell uid.
package com.clipsync;

import android.content.AttributionSource;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.Process;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

public final class ClipHelper implements ClipboardManager.OnPrimaryClipChangedListener, Runnable, Handler.Callback {
    private static final int MAX_BYTES = 65536;

    private static WifiManager.MulticastLock multicastLock;

    private final ClipboardManager clipboard;
    private final Handler handler;
    private final OutputStream stdout;
    private final ClipboardEvents events = new ClipboardEvents();
    private boolean applying;

    @SuppressWarnings("deprecation")
    public static void main(String[] args) {
        exemptHiddenApi();
        try {
            Looper.prepareMainLooper();
        } catch (Throwable thrown) {
            die("looper", thrown);
            return;
        }
        Context context = shellContext(systemContext());
        if (context == null) {
            System.exit(1);
            return;
        }
        ClipboardManager manager = (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
        if (manager == null) {
            die("clipboard", null);
            return;
        }
        holdMulticast(context);
        ClipHelper helper = new ClipHelper(manager);
        manager.addPrimaryClipChangedListener(helper);
        Thread input = new Thread(helper);
        input.setDaemon(true);
        input.start();
        Looper.loop();
    }

    private ClipHelper(ClipboardManager clipboard) {
        this.clipboard = clipboard;
        this.handler = new Handler(Looper.getMainLooper(), this);
        this.stdout = new FileOutputStream(FileDescriptor.out);
    }

    @Override
    public boolean handleMessage(Message message) {
        applyText((String) message.obj);
        return true;
    }

    @Override
    public void onPrimaryClipChanged() {
        if (applying) {
            return;
        }
        ClipData data;
        try {
            data = clipboard.getPrimaryClip();
        } catch (Throwable thrown) {
            System.err.println(thrown);
            return;
        }
        if (data == null || data.getItemCount() < 1) {
            events.clear();
            writeFrame("C");
            return;
        }
        CharSequence text = data.getItemAt(0).getText();
        if (text == null || text.length() == 0) {
            events.clear();
            writeFrame("C");
            return;
        }
        String value = text.toString();
        if (events.local(data.getDescription().getTimestamp(), data.getDescription().getLabel(), value)) {
            writeFrame("L" + value);
        }
    }

    @Override
    public void run() {
        DataInputStream input = new DataInputStream(new FileInputStream(FileDescriptor.in));
        try {
            while (true) {
                int length = input.readInt();
                if (length <= 17 || length > MAX_BYTES + 17) {
                    die("frame", null);
                    return;
                }
                byte[] body = new byte[length];
                input.readFully(body);
                handler.obtainMessage(0, new String(body, StandardCharsets.UTF_8)).sendToTarget();
            }
        } catch (EOFException ignored) {
            System.exit(0);
        } catch (IOException thrown) {
            die("stdin", thrown);
        }
    }

    private void applyText(String command) {
        if (command.length() <= 17 || command.charAt(0) != 'W') {
            die("command", null);
            return;
        }
        String id = command.substring(1, 17);
        String result = "E" + id;
        applying = true;
        try {
            String label = "ClipSync/" + id;
            String text = command.substring(17);
            clipboard.setPrimaryClip(ClipData.newPlainText(label, text));
            events.applied(label, text);
            result = "A" + id;
        } catch (Throwable thrown) {
            System.err.println(thrown);
        } finally {
            applying = false;
        }
        writeFrame(result);
    }

    private void writeFrame(String value) {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        if (body.length == 0 || body.length > MAX_BYTES + 17 || (value.charAt(0) == 'L' && body.length > MAX_BYTES + 1)) {
            return;
        }
        byte[] packet = new byte[4 + body.length];
        packet[0] = (byte) (body.length >>> 24);
        packet[1] = (byte) (body.length >>> 16);
        packet[2] = (byte) (body.length >>> 8);
        packet[3] = (byte) body.length;
        System.arraycopy(body, 0, packet, 4, body.length);
        try {
            stdout.write(packet);
            stdout.flush();
        } catch (IOException thrown) {
            die("stdout", thrown);
        }
    }

    private static void die(String what, Throwable thrown) {
        System.err.println(what);
        if (thrown != null) {
            System.err.println(thrown.toString());
        }
        System.exit(1);
    }

    private static void exemptHiddenApi() {
        try {
            Method forName = Class.class.getDeclaredMethod("forName", String.class);
            Method getDeclaredMethod = Class.class.getDeclaredMethod("getDeclaredMethod", String.class, Class[].class);
            Class<?> vmRuntime = (Class<?>) forName.invoke(null, "dalvik.system.VMRuntime");
            Method getRuntime = (Method) getDeclaredMethod.invoke(vmRuntime, "getRuntime", new Class<?>[0]);
            Method setExemptions = (Method) getDeclaredMethod.invoke(
                    vmRuntime, "setHiddenApiExemptions", new Class<?>[] {String[].class});
            Object runtime = getRuntime.invoke(null);
            setExemptions.invoke(runtime, (Object) new String[] {"L"});
        } catch (Throwable thrown) {
            System.err.println(thrown);
        }
    }

    private static void holdMulticast(Context context) {
        try {
            WifiManager wifi = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
            if (wifi == null) {
                return;
            }
            multicastLock = wifi.createMulticastLock("clipsync");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Throwable thrown) {
            System.err.println(thrown);
        }
    }

    private static Context shellContext(Context system) {
        if (system == null) {
            return null;
        }
        try {
            Context shell = system.createPackageContext("com.android.shell", 0);
            AttributionSource source = new AttributionSource.Builder(Process.myUid()).setPackageName("com.android.shell").build();
            Class<?> type = shell.getClass();
            while (type != null) {
                try {
                    Field field = type.getDeclaredField("mAttributionSource");
                    field.setAccessible(true);
                    field.set(shell, source);
                } catch (NoSuchFieldException ignored) {
                }
                type = type.getSuperclass();
            }
            return shell;
        } catch (Throwable thrown) {
            die("shell", thrown);
            return null;
        }
    }

    private static Context systemContext() {
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Object thread = activityThread.getMethod("systemMain").invoke(null);
            return (Context) activityThread.getMethod("getSystemContext").invoke(thread);
        } catch (Throwable thrown) {
            die("context", thrown);
            return null;
        }
    }
}
