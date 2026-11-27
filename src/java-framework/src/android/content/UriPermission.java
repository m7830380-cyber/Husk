package android.content;

/** A content URI an app kept access to (ContentResolver.takePersistableUriPermission): Husk keeps the list in the app's data. */
public final class UriPermission implements android.os.Parcelable {
    public static android.os.Parcelable.Creator CREATOR = husk.StubCreator.of(UriPermission.class);
    public static final long INVALID_TIME = Long.MIN_VALUE;
    private final android.net.Uri mUri;
    private final int mFlags;
    private final long mTime;
    public UriPermission(android.net.Uri uri, int flags, long time) { mUri = uri; mFlags = flags; mTime = time; }
    public UriPermission(android.os.Parcel p) { this(null, 0, INVALID_TIME); }
    UriPermission() { this(null, 0, INVALID_TIME); }
    public int describeContents() { return 0; }
    public long getPersistedTime() { return mTime; }
    public android.net.Uri getUri() { return mUri; }
    public boolean isReadPermission() { return (mFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0; }
    public boolean isWritePermission() { return (mFlags & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0; }
    public void writeToParcel(android.os.Parcel p, int flags) {}
    @Override public String toString() { return "UriPermission {uri=" + mUri + ", modeFlags=" + mFlags + ", persistedTime=" + mTime + "}"; }
}
