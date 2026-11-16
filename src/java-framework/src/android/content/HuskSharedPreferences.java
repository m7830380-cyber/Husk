package android.content;

import java.io.*;
import java.util.*;

/** SharedPreferences kept in a file of typed lines ("i key value"), written on every commit. */
public final class HuskSharedPreferences implements SharedPreferences {
    private final File file;
    private final HashMap<String, Object> map = new HashMap<>();
    private final java.util.concurrent.CopyOnWriteArrayList<OnSharedPreferenceChangeListener> listeners = new java.util.concurrent.CopyOnWriteArrayList<>();
    private static final android.os.Handler MAIN = new android.os.Handler(android.os.Looper.getMainLooper());
    private static final java.util.concurrent.ExecutorService WRITER = java.util.concurrent.Executors.newSingleThreadExecutor(r -> { Thread t = new Thread(r, "SharedPreferencesWriter"); t.setDaemon(true); return t; });
    private final java.util.concurrent.atomic.AtomicBoolean writePending = new java.util.concurrent.atomic.AtomicBoolean();

    public HuskSharedPreferences(File f) { file = f; load(); }

    private static String esc(String s) { return s.replace("\\", "\\\\").replace("\n", "\\n"); }
    private static String unesc(String s) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) { char c = s.charAt(i); if (c == '\\' && i + 1 < s.length()) { char n = s.charAt(++i); b.append(n == 'n' ? '\n' : n); } else b.append(c); }
        return b.toString();
    }
    private void loadXml(File f) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), "UTF-8"))) {
            String line;
            String curName = null;
            HashSet<String> curSet = null;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("<string name=\"")) {
                    int n1 = 14, n2 = line.indexOf('"', n1);
                    if (n2 > 0) {
                        String name = line.substring(n1, n2);
                        int v1 = line.indexOf('>', n2) + 1;
                        int v2 = line.lastIndexOf('<');
                        if (v1 > 0 && v2 >= v1) map.put(name, line.substring(v1, v2));
                        else if (line.endsWith("/>")) map.put(name, "");
                    }
                } else if (line.startsWith("<int name=\"")) {
                    parseXmlAttr(line, "int");
                } else if (line.startsWith("<long name=\"")) {
                    parseXmlAttr(line, "long");
                } else if (line.startsWith("<float name=\"")) {
                    parseXmlAttr(line, "float");
                } else if (line.startsWith("<boolean name=\"")) {
                    parseXmlAttr(line, "boolean");
                } else if (line.startsWith("<set name=\"")) {
                    int n1 = 11, n2 = line.indexOf('"', n1);
                    if (n2 > 0) {
                        curName = line.substring(n1, n2);
                        curSet = new HashSet<>();
                    }
                } else if (line.startsWith("<string>") && curSet != null) {
                    int v1 = 8, v2 = line.lastIndexOf('<');
                    if (v2 >= v1) curSet.add(line.substring(v1, v2));
                } else if (line.startsWith("</set>") && curName != null && curSet != null) {
                    map.put(curName, curSet);
                    curName = null;
                    curSet = null;
                }
            }
        } catch (Exception ignored) { }
    }

    private void parseXmlAttr(String line, String type) {
        try {
            int n1 = line.indexOf("name=\"") + 6;
            int n2 = line.indexOf('"', n1);
            int v1 = line.indexOf("value=\"") + 7;
            int v2 = line.indexOf('"', v1);
            if (n1 > 5 && n2 > n1 && v1 > 6 && v2 > v1) {
                String name = line.substring(n1, n2);
                String val = line.substring(v1, v2);
                switch (type) {
                case "int": map.put(name, Integer.parseInt(val)); break;
                case "long": map.put(name, Long.parseLong(val)); break;
                case "float": map.put(name, Float.parseFloat(val)); break;
                case "boolean": map.put(name, Boolean.parseBoolean(val)); break;
                }
            }
        } catch (Exception ignored) { }
    }

    private void load() {
        if (!file.exists()) return;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(file), "UTF-8"))) {
            String line;
            boolean firstLine = true;
            while ((line = r.readLine()) != null) {
                if (firstLine) {
                    firstLine = false;
                    String tr = line.trim();
                    if (tr.startsWith("<?xml") || tr.startsWith("<map")) {
                        loadXml(file);
                        return;
                    }
                }
                if (line.length() < 3) continue;
                char t = line.charAt(0);
                int sp = -1;
                for (int i = 2; i < line.length(); i++) {
                    if (line.charAt(i) == ' ') {
                        int backslashes = 0;
                        for (int j = i - 1; j >= 0 && line.charAt(j) == '\\'; j--) {
                            backslashes++;
                        }
                        if ((backslashes % 2) == 0) {
                            sp = i;
                            break;
                        }
                    }
                }
                if (sp < 0) continue;
                String k = unesc(line.substring(2, sp).replace("\\ ", " "));
                String v = line.substring(sp + 1);
                try {
                    switch (t) {
                    case 's': map.put(k, unesc(v)); break;
                    case 'i': map.put(k, Integer.parseInt(v.trim())); break;
                    case 'l': map.put(k, Long.parseLong(v.trim())); break;
                    case 'f': map.put(k, Float.parseFloat(v.trim())); break;
                    case 'b': map.put(k, Boolean.parseBoolean(v.trim())); break;
                    case 'S': { HashSet<String> set = new HashSet<>(); for (String p : v.split("\u0001", -1)) if (!p.isEmpty()) set.add(unesc(p)); map.put(k, set); break; }
                    default: map.put(k, unesc(v)); break;
                    }
                } catch (Exception ex) {
                    map.put(k, unesc(v));
                }
            }
        } catch (Exception ignored) { }
    }
    private synchronized boolean save() {
        file.getParentFile().mkdirs();
        try (Writer w = new OutputStreamWriter(new FileOutputStream(file), "UTF-8")) {
            for (Map.Entry<String, Object> e : map.entrySet()) {
                Object v = e.getValue(); String k = esc(e.getKey()).replace(" ", "\\ ");
                if (v instanceof String) w.write("s " + k + " " + esc((String) v) + "\n");
                else if (v instanceof Integer) w.write("i " + k + " " + v + "\n");
                else if (v instanceof Long) w.write("l " + k + " " + v + "\n");
                else if (v instanceof Float) w.write("f " + k + " " + v + "\n");
                else if (v instanceof Boolean) w.write("b " + k + " " + v + "\n");
                else if (v instanceof Set) { StringBuilder b = new StringBuilder(); for (Object o : (Set<?>) v) b.append(esc(String.valueOf(o))).append('\u0001'); w.write("S " + k + " " + b + "\n"); }
            }
            return true;
        } catch (IOException e) { return false; }
    }
    public synchronized Map<String, ?> getAll() { return new HashMap<>(map); }
    public synchronized String getString(String k, String d) { Object v = map.get(k); return v instanceof String ? (String) v : d; }
    @SuppressWarnings("unchecked") public synchronized Set<String> getStringSet(String k, Set<String> d) { Object v = map.get(k); return v instanceof Set ? (Set<String>) v : d; }
    public synchronized int getInt(String k, int d) { Object v = map.get(k); return v instanceof Integer ? (Integer) v : d; }
    public synchronized long getLong(String k, long d) { Object v = map.get(k); return v instanceof Long ? (Long) v : v instanceof Integer ? (Integer) v : d; }
    public synchronized float getFloat(String k, float d) { Object v = map.get(k); return v instanceof Float ? (Float) v : d; }
    public synchronized boolean getBoolean(String k, boolean d) { Object v = map.get(k); return v instanceof Boolean ? (Boolean) v : d; }
    public synchronized boolean contains(String k) { return map.containsKey(k); }
    public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) { if (l != null && !listeners.contains(l)) listeners.add(l); }
    public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) { listeners.remove(l); }
    /* As Android's: an editor's changes go to memory at once (under the preferences' lock), apply() writes the file in the
       background (one write for a burst), listeners hear each changed key on the main thread. Editors may be shared between threads. */
    private void notifyChanged(List<String> keys) {
        if (listeners.isEmpty() || keys.isEmpty()) return;
        Runnable r = () -> { for (OnSharedPreferenceChangeListener l : listeners) for (String k : keys) l.onSharedPreferenceChanged(this, k); };
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) r.run(); else MAIN.post(r);
    }
    private List<String> toMemory(boolean clear, Set<String> removes, Map<String, Object> puts) {
        ArrayList<String> changed = new ArrayList<>();
        synchronized (this) {
            if (clear && !map.isEmpty()) map.clear();
            for (String k : removes) if (map.remove(k) != null) changed.add(k);
            for (Map.Entry<String, Object> e : puts.entrySet()) {
                Object old = e.getValue() == null ? map.remove(e.getKey()) : map.put(e.getKey(), e.getValue());
                if (!Objects.equals(old, e.getValue())) changed.add(e.getKey());
            }
        }
        return changed;
    }
    /** As QueuedWork.waitToFinish: apply()'s pending writes done before an activity pauses or stops (the app may be killed after). */
    public static void flushAll() { try { WRITER.submit(() -> {}).get(2, java.util.concurrent.TimeUnit.SECONDS); } catch (Exception e) {} }
    public Editor edit() {
        return new Editor() {
            private final HashMap<String, Object> puts = new HashMap<>();
            private final HashSet<String> removes = new HashSet<>();
            private boolean clear;
            public synchronized Editor putString(String k, String v) { puts.put(k, v); return this; }
            public synchronized Editor putStringSet(String k, Set<String> v) { puts.put(k, v == null ? null : new HashSet<>(v)); return this; }
            public synchronized Editor putInt(String k, int v) { puts.put(k, v); return this; }
            public synchronized Editor putLong(String k, long v) { puts.put(k, v); return this; }
            public synchronized Editor putFloat(String k, float v) { puts.put(k, v); return this; }
            public synchronized Editor putBoolean(String k, boolean v) { puts.put(k, v); return this; }
            public synchronized Editor remove(String k) { removes.add(k); return this; }
            public synchronized Editor clear() { clear = true; return this; }
            private List<String> takeAndApply() {
                boolean c; HashSet<String> r; HashMap<String, Object> p;
                synchronized (this) { c = clear; r = new HashSet<>(removes); p = new HashMap<>(puts); clear = false; removes.clear(); puts.clear(); }
                return toMemory(c, r, p);
            }
            public boolean commit() { List<String> ch = takeAndApply(); boolean ok = save(); notifyChanged(ch); return ok; }
            public void apply() {
                List<String> ch = takeAndApply();
                if (writePending.compareAndSet(false, true)) WRITER.execute(() -> { writePending.set(false); save(); });
                notifyChanged(ch);
            }
        };
    }
}
