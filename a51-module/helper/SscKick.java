// Get UHQ after the buds connect on their own (plan-phase5-a51.md T6, F56, F57).
// The buds always pick basic 48k (0x0c) when they start the connection. When the phone
// starts it, the patched stack sends 0x0e (UHQ). This helper reconnects the buds from the
// phone side: BluetoothDevice.disconnect() then connect(), the same as tapping the buds in
// Settings.
//
// Run as root: CLASSPATH=ssckick.dex app_process / SscKick status|reconnect [NAME]
// NAME is a substring of the bonded device name (default "Buds").
// Uses reflection only, so it compiles against a plain JDK; d8 turns it into dex.
//
// Routes that don't work on this build (F57): getProfileProxy needs a registered app
// process ("Unable to find app for caller"); bindBluetoothProfileService(WithCaller)
// returns false for A2DP.
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;

public class SscKick {
    static Object call(Object o, String name, Object... args) throws Exception {
        for (Method m : o.getClass().getMethods()) {
            if (m.getName().equals(name) && m.getParameterCount() == args.length) {
                return m.invoke(o, args);
            }
        }
        throw new NoSuchMethodException(o.getClass().getName() + "." + name);
    }

    public static void main(String[] argv) throws Exception {
        String mode = argv.length > 0 ? argv[0] : "status";
        String match = argv.length > 1 ? argv[1] : "Buds";

        Class.forName("android.os.Looper").getMethod("prepareMainLooper").invoke(null);
        Class<?> at = Class.forName("android.app.ActivityThread");
        // Wires up BluetoothFrameworkInitializer (Bluetooth is a mainline module since T).
        at.getMethod("initializeMainlineModules").invoke(null);
        at.getMethod("systemMain").invoke(null);

        Object adapter = Class.forName("android.bluetooth.BluetoothAdapter")
                .getMethod("getDefaultAdapter").invoke(null);
        List<Object> targets = new ArrayList<>();
        for (Object dev : (Set<?>) call(adapter, "getBondedDevices")) {
            String name = String.valueOf(call(dev, "getName"));
            boolean connected = (Boolean) call(dev, "isConnected");
            System.out.println(dev + " " + name + " connected=" + connected);
            if (connected && name.contains(match)) targets.add(dev);
        }
        if ("reconnect".equals(mode)) {
            for (Object dev : targets) {
                System.out.println("disconnect " + dev + ": " + call(dev, "disconnect"));
                Thread.sleep(3000);
                System.out.println("connect " + dev + ": " + call(dev, "connect"));
            }
        }
        System.exit(0);
    }
}
