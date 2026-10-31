package com.battery.analysis

import android.app.Application
import android.os.Build
import android.os.Environment
import android.util.Log
import com.battery.analysis.manager.AppLifecycleTracker
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 全局 Application 类，负责生命周期追踪初始化与全局防崩 Crash Handler。
 */
class BatteryApplication : Application() {

    override fun onCreate() {
        super.onCreate()
        setupCrashHandler()
        AppLifecycleTracker.init(this)
    }

    private fun setupCrashHandler() {
        val defaultHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                Log.e(TAG, "FATAL CRASH intercepted on thread [${thread.name}]", throwable)
                saveCrashLog(thread, throwable)
            } catch (e: Throwable) {
                e.printStackTrace()
            } finally {
                defaultHandler?.uncaughtException(thread, throwable)
            }
        }
    }

    private fun saveCrashLog(thread: Thread, throwable: Throwable) {
        val sw = StringWriter()
        throwable.printStackTrace(PrintWriter(sw))
        val stackTrace = sw.toString()

        val timeStr = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault()).format(Date())
        val logContent = buildString {
            appendLine("=== Battery App Crash Report ===")
            appendLine("Time: $timeStr")
            appendLine("Device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE})")
            appendLine("Android: ${Build.VERSION.RELEASE} (SDK ${Build.VERSION.SDK_INT})")
            appendLine("Thread: ${thread.name} (id: ${thread.id})")
            appendLine("Exception: ${throwable.javaClass.name}: ${throwable.message}")
            appendLine("StackTrace:")
            appendLine(stackTrace)
            appendLine("===============================")
        }

        // 1. Internal filesDir
        try {
            val internalFile = File(filesDir, "last_crash.log")
            internalFile.writeText(logContent)
        } catch (_: Throwable) {}

        // 2. External app filesDir
        try {
            val extDir = getExternalFilesDir(null)
            if (extDir != null) {
                File(extDir, "crash.log").writeText(logContent)
            }
        } catch (_: Throwable) {}

        // 3. /sdcard/Download
        try {
            val downloadDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
            if (downloadDir != null && downloadDir.exists()) {
                File(downloadDir, "battery_analysis_crash.log").writeText(logContent)
            }
        } catch (_: Throwable) {}
    }

    companion object {
        private const val TAG = "BatteryApp"
    }
}
