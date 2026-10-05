# src/android/android_jni.cc finds these by name.
-keep class com.telosnex.audio.AudioRoutes {
    static java.lang.String devices();
    static boolean setCommunication(int, int, int);
    static native void nativeDevicesChanged();
}
