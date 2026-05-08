# Proguard rules for HearingAid app
# Keep JNI-linked classes (NativeAudioEngine) intact
-keep class com.hearingaid.app.NativeAudioEngine { *; }
-keep class com.hearingaid.app.AudioProcessingService { *; }
-keep class com.hearingaid.app.MainActivity { *; }

# MPAndroidChart
-keep class com.github.mikephil.charting.** { *; }
