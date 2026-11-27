package husk;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.util.ArrayList;

/**
 * The system's file picker (GET_CONTENT, OPEN_DOCUMENT, PICK): the host shows the iOS document picker and copies what the user
 * picks into the app's cache; the caller gets content://husk.documents/document/<path> URIs, which this provider opens and
 * describes (display name, size, MIME type) like Android's documents providers do.
 * A folder (OPEN_DOCUMENT_TREE) is picked in Husk's own picker, inside Husk's Documents: the caller gets
 * content://husk.documents/tree/<path>, lists it through DocumentsContract's child URIs, and reads, writes, makes and deletes
 * the files in it, as with Android's external storage provider.
 */
public final class Documents extends ContentProvider {
    public static final String AUTHORITY = "husk.documents";
    private static final Documents sSelf = new Documents();
    private static Activity sCaller;
    private static int sRequest;
    private static boolean sTree;
    static final String DIR = "vnd.android.document/directory";
    private static final ArrayList<String> sPicked = new ArrayList<>();

    static { ContentResolver.register(AUTHORITY, sSelf); }

    /** Whether the intent asks for the system's file picker. */
    public static boolean handles(Intent i) {
        String a = i.getAction();
        if (Intent.ACTION_GET_CONTENT.equals(a) || Intent.ACTION_OPEN_DOCUMENT.equals(a) || Intent.ACTION_OPEN_DOCUMENT_TREE.equals(a)) return true;
        if (!Intent.ACTION_PICK.equals(a)) return false;
        // PICK from the gallery (images, videos) or with only a type; contacts and other pickers are not files
        Uri d = i.getData();
        String t = i.getType();
        return d == null ? t != null && (t.startsWith("image/") || t.startsWith("video/") || t.startsWith("audio/") || t.equals("*/*"))
                         : "media".equals(d.getAuthority());
    }

    /** Opens the host's picker for the caller's request; the answer comes back through deliver(). */
    public static void pick(Activity caller, Intent i, int request) {
        if (sCaller != null) finishPick(false);   // a second request replaces the first, which hears the user backed out
        sCaller = caller;
        sRequest = request;
        sPicked.clear();
        sTree = Intent.ACTION_OPEN_DOCUMENT_TREE.equals(i.getAction());
        if (sTree) {
            android.util.Log.d("Husk", "folder picker");
            Native.pickDocuments(DIR, false);
            return;
        }
        StringBuilder types = new StringBuilder();
        String[] extra = i.getStringArrayExtra(Intent.EXTRA_MIME_TYPES);
        if (extra != null) for (String t : extra) { if (types.length() > 0) types.append(','); types.append(t); }
        if (types.length() == 0) types.append(i.getType() != null ? i.getType() : "*/*");
        boolean multiple = i.getBooleanExtra(Intent.EXTRA_ALLOW_MULTIPLE, false);
        android.util.Log.d("Husk", "file picker for " + types + (multiple ? " (several)" : ""));
        Native.pickDocuments(types.toString(), multiple);
    }

    /** One item of the host's answer: "f<path>" a picked file, "F" the picker closed. */
    static void deliver(String item) {
        if (item.charAt(0) == 'f') sPicked.add(item.substring(1));
        else finishPick(true);
    }

    private static void finishPick(boolean answer) {
        Activity caller = sCaller;
        int request = sRequest;
        sCaller = null;
        if (caller == null) return;
        if (!answer || sPicked.isEmpty()) { caller.huskActivityResult(request, Activity.RESULT_CANCELED, null); return; }
        Intent data = new Intent();
        if (sTree) {
            Uri tree = android.provider.DocumentsContract.buildTreeDocumentUri(AUTHORITY, new File(sPicked.get(0)).getAbsolutePath());
            sPicked.clear();
            data.setData(tree);
            data.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
            android.util.Log.d("Husk", "folder picked: " + tree);
            caller.huskActivityResult(request, Activity.RESULT_OK, data);
            return;
        }
        Uri first = uriFor(new File(sPicked.get(0)));
        data.setData(first);
        ClipData clip = ClipData.newRawUri(null, first);
        for (int k = 1; k < sPicked.size(); k++) clip.addItem(new ClipData.Item(uriFor(new File(sPicked.get(k)))));
        data.setClipData(clip);
        data.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        sPicked.clear();
        caller.huskActivityResult(request, Activity.RESULT_OK, data);
    }

    public static Uri uriFor(File f) {
        return new Uri.Builder().scheme(ContentResolver.SCHEME_CONTENT).authority(AUTHORITY).appendPath("document").appendPath(f.getAbsolutePath()).build();
    }
    /** The file a URI names: document/<path>, tree/<path>, tree/<root>/document/<path>, each maybe followed by /children */
    private static File fileOf(Uri u) {
        java.util.List<String> seg = u.getPathSegments();
        if (seg.size() >= 4 && "tree".equals(seg.get(0)) && "document".equals(seg.get(2))) return new File(seg.get(3));
        if (seg.size() >= 2 && ("tree".equals(seg.get(0)) || "document".equals(seg.get(0)))) return new File(seg.get(1));
        String enc = u.getEncodedPath();
        if (enc != null && enc.startsWith("/document/")) return new File(Uri.decode(enc.substring("/document/".length())));
        return seg.size() >= 2 ? new File(seg.get(seg.size() - 1)) : new File(u.getPath());
    }
    private static boolean isChildren(Uri u) {
        java.util.List<String> seg = u.getPathSegments();
        return seg.size() >= 3 && "children".equals(seg.get(seg.size() - 1));
    }
    private static String mimeOf(File f) {
        if (f.isDirectory()) return DIR;
        String t = java.net.URLConnection.guessContentTypeFromName(f.getName());
        return t != null ? t : "application/octet-stream";
    }
    private static Object[] row(File f, String[] cols) {
        Object[] row = new Object[cols.length];
        for (int k = 0; k < cols.length; k++) {
            switch (cols[k]) {
            case "_display_name": row[k] = f.getName(); break;
            case "_size": row[k] = f.isDirectory() ? 0L : f.length(); break;
            case "mime_type": row[k] = mimeOf(f); break;
            case "document_id": case "_id": row[k] = f.getAbsolutePath(); break;
            case "last_modified": case "date_modified": row[k] = f.lastModified(); break;
            // write, delete, rename; a folder also takes new files
            case "flags": row[k] = 0x2 | 0x4 | 0x40 | (f.isDirectory() ? 0x8 : 0); break;
            case "_data": row[k] = f.getAbsolutePath(); break;
            default: row[k] = null;
            }
        }
        return row;
    }

    // ---- the provider
    @Override public boolean onCreate() { return true; }
    @Override public Cursor query(Uri u, String[] proj, String sel, String[] args, String sort) {
        File f = fileOf(u);
        String[] cols = proj != null ? proj : new String[] { "document_id", "_display_name", "mime_type", "_size", "last_modified", "flags" };
        MatrixCursor c = new MatrixCursor(cols);
        if (!f.exists()) return c;
        if (isChildren(u)) {
            File[] kids = f.listFiles();
            if (kids != null) {
                java.util.Arrays.sort(kids, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
                for (File k : kids) if (!k.getName().startsWith(".")) c.addRow(row(k, cols));
            }
            return c;
        }
        c.addRow(row(f, cols));
        return c;
    }
    @Override public String getType(Uri u) { return mimeOf(fileOf(u)); }
    /** DocumentsContract's createDocument, deleteDocument and renameDocument */
    @Override public android.os.Bundle call(String method, String arg, android.os.Bundle extras) {
        if (extras == null) return null;
        Uri u = (Uri) extras.getParcelable("uri");
        if (u == null) return null;
        File f = fileOf(u);
        android.os.Bundle out = new android.os.Bundle();
        switch (method) {
        case "android:createDocument": {
            String name = extras.getString("_display_name", "untitled"), mime = extras.getString("mime_type", "");
            File n = new File(f, name);
            for (int k = 1; n.exists(); k++) {
                int dot = name.lastIndexOf('.');
                n = new File(f, dot > 0 ? name.substring(0, dot) + " (" + k + ")" + name.substring(dot) : name + " (" + k + ")");
            }
            try {
                boolean ok = DIR.equals(mime) ? n.mkdirs() : n.createNewFile();
                if (!ok) return null;
            } catch (java.io.IOException e) { return null; }
            out.putParcelable("uri", sibling(u, n));
            return out;
        }
        case "android:deleteDocument": case "android:removeDocument":
            return deleteAll(f) ? out : null;
        case "android:renameDocument": {
            File n = new File(f.getParentFile(), extras.getString("_display_name", f.getName()));
            if (!f.renameTo(n)) return null;
            out.putParcelable("uri", sibling(u, n));
            return out;
        }
        }
        return null;
    }
    /** a URI for another file, in the same tree as u when u is in one */
    private static Uri sibling(Uri u, File f) {
        java.util.List<String> seg = u.getPathSegments();
        if (seg.size() >= 2 && "tree".equals(seg.get(0))) return android.provider.DocumentsContract.buildDocumentUriUsingTree(u, f.getAbsolutePath());
        return uriFor(f);
    }
    private static boolean deleteAll(File f) {
        File[] kids = f.isDirectory() ? f.listFiles() : null;
        if (kids != null) for (File k : kids) deleteAll(k);
        return f.delete();
    }
    @Override public ParcelFileDescriptor openFile(Uri u, String mode) throws java.io.FileNotFoundException {
        return ParcelFileDescriptor.open(fileOf(u), ParcelFileDescriptor.parseMode(mode));
    }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String sel, String[] args) { return fileOf(u).delete() ? 1 : 0; }
    @Override public int update(Uri u, ContentValues v, String sel, String[] args) { return 0; }
}
