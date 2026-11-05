package com.telosnex.audio;

import android.Manifest;
import android.content.pm.PackageManager;

import androidx.annotation.NonNull;

import io.flutter.embedding.engine.plugins.FlutterPlugin;
import io.flutter.embedding.engine.plugins.activity.ActivityAware;
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding;
import io.flutter.plugin.common.MethodCall;
import io.flutter.plugin.common.MethodChannel;
import io.flutter.plugin.common.PluginRegistry;

import java.util.ArrayList;
import java.util.List;

/**
 * Gives the native engine its Android routes (see {@link AudioRoutes}), and
 * asks for the microphone: the channel "telosnex_audio/permission" has
 * "status" and "request", which answer "granted", "denied" or
 * "notDetermined" (lib/src/engine_native.dart).
 */
public final class TelosnexAudioPlugin
        implements FlutterPlugin,
                ActivityAware,
                MethodChannel.MethodCallHandler,
                PluginRegistry.RequestPermissionsResultListener {
    // Flutter's activity passes on request codes below 0x10000.
    private static final int REQUEST_CODE = 0x7473;

    private MethodChannel channel;
    private ActivityPluginBinding activity;
    // Requests waiting for the answer of the prompt that is showing.
    private final List<MethodChannel.Result> pending = new ArrayList<>();

    @Override
    public void onAttachedToEngine(@NonNull FlutterPluginBinding binding) {
        AudioRoutes.attach(binding.getApplicationContext());
        channel = new MethodChannel(binding.getBinaryMessenger(), "telosnex_audio/permission");
        channel.setMethodCallHandler(this);
    }

    @Override
    public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
        channel.setMethodCallHandler(null);
        channel = null;
    }

    @Override
    public void onMethodCall(@NonNull MethodCall call, @NonNull MethodChannel.Result result) {
        switch (call.method) {
            case "status":
                // Android does not say whether a request would show the
                // prompt ("don't ask again"), so not granted is
                // notDetermined until a request answers.
                result.success(AudioRoutes.micGranted() ? "granted" : "notDetermined");
                break;
            case "request":
                request(result);
                break;
            default:
                result.notImplemented();
        }
    }

    private void request(MethodChannel.Result result) {
        if (AudioRoutes.micGranted()) {
            result.success("granted");
            return;
        }
        if (activity == null) {
            result.error("no_activity", "The microphone prompt needs an activity", null);
            return;
        }
        pending.add(result);
        if (pending.size() > 1) return;  // a prompt is showing
        activity.getActivity()
                .requestPermissions(new String[] {Manifest.permission.RECORD_AUDIO}, REQUEST_CODE);
    }

    @Override
    public boolean onRequestPermissionsResult(
            int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        if (requestCode != REQUEST_CODE) return false;
        // An empty result: the request was interrupted. The status says
        // what the user chose, if anything.
        final boolean granted =
                grantResults.length > 0
                        ? grantResults[0] == PackageManager.PERMISSION_GRANTED
                        : AudioRoutes.micGranted();
        final String answer = granted ? "granted" : "denied";
        for (MethodChannel.Result r : pending) r.success(answer);
        pending.clear();
        return true;
    }

    private void attach(ActivityPluginBinding binding) {
        activity = binding;
        binding.addRequestPermissionsResultListener(this);
    }

    private void detach() {
        if (activity != null) activity.removeRequestPermissionsResultListener(this);
        activity = null;
    }

    @Override
    public void onAttachedToActivity(@NonNull ActivityPluginBinding binding) {
        attach(binding);
    }

    // A configuration change: the answer goes to the new activity, so the
    // pending requests stay.
    @Override
    public void onDetachedFromActivityForConfigChanges() {
        detach();
    }

    @Override
    public void onReattachedToActivityForConfigChanges(@NonNull ActivityPluginBinding binding) {
        attach(binding);
    }

    @Override
    public void onDetachedFromActivity() {
        detach();
        for (MethodChannel.Result r : pending) {
            r.error("no_activity", "The activity ended before the answer", null);
        }
        pending.clear();
    }
}
