#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <android/log.h>

#define TAG "PowerReaderJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define BUFFER_SIZE 64

/**
 * 底层 POSIX 文件描述符缓存结构体，用于保持内核节点长期打开 (O_RDONLY | O_CLOEXEC)，
 * 通过 pread(fd, buf, size, 0) 无锁、零堆内存分配、零用户态缓冲直读内核数据。
 */
typedef struct {
    int voltage_fd;
    int current_fd;
    int capacity_fd;
    int status_fd;
    int temp_fd;
    int initialized;
    int init_attempted;
} FdCache;

static FdCache g_cache = {-1, -1, -1, -1, -1, 0, 0};

/**
 * 候选路径列表，全量覆盖高通、联发科、三星、小米/HyperOS、华为及主流国产 OEM 的电源节点。
 */
static const char *VOLTAGE_CANDIDATES[] = {
    "/sys/class/power_supply/battery/voltage_now",
    "/sys/class/power_supply/Battery/voltage_now",
    "/sys/class/power_supply/bms/voltage_now",
    "/sys/class/power_supply/battery_gauge/voltage_now",
    "/sys/class/power_supply/bq27z561-0/voltage_now",
    "/sys/class/power_supply/sc8545-standalone/voltage_now",
    "/sys/class/power_supply/qcom-battery/voltage_now",
    "/sys/class/power_supply/battery/batt_vol",
    "/sys/class/power_supply/battery/voltage_avg",
    "/sys/class/power_supply/main/voltage_now",
    "/sys/class/power_supply/parallel/voltage_now",
    "/sys/class/power_supply/bk_battery/voltage_now",
    "/sys/class/power_supply/mtk-gauge/voltage_now",
    "/sys/class/power_supply/mtk_gauge/voltage_now",
    "/sys/class/power_supply/primary_battery/voltage_now",
    NULL
};

static const char *CURRENT_CANDIDATES[] = {
    "/sys/class/power_supply/battery/current_now",
    "/sys/class/power_supply/Battery/current_now",
    "/sys/class/power_supply/bms/current_now",
    "/sys/class/power_supply/battery_gauge/current_now",
    "/sys/class/power_supply/bq27z561-0/current_now",
    "/sys/class/power_supply/sc8545-standalone/current_now",
    "/sys/class/power_supply/qcom-battery/current_now",
    "/sys/class/power_supply/battery/current_avg",
    "/sys/class/power_supply/Battery/current_avg",
    "/sys/class/power_supply/battery/batt_current",
    "/sys/class/power_supply/battery/input_current_now",
    "/sys/class/power_supply/parallel/current_now",
    "/sys/class/power_supply/main/current_now",
    "/sys/class/power_supply/bk_battery/current_now",
    "/sys/class/power_supply/mtk-gauge/current_now",
    "/sys/class/power_supply/mtk_gauge/current_now",
    "/sys/class/power_supply/mtk-battery/current_now",
    "/sys/class/power_supply/mtk_battery/current_now",
    "/sys/class/power_supply/battery/hw_current_now",
    "/sys/class/power_supply/Battery/hw_current_now",
    "/sys/class/power_supply/battery/fg_current",
    "/sys/class/power_supply/battery/current_meas",
    "/sys/class/power_supply/primary_battery/current_now",
    NULL
};

static const char *CAPACITY_CANDIDATES[] = {
    "/sys/class/power_supply/battery/capacity",
    "/sys/class/power_supply/Battery/capacity",
    "/sys/class/power_supply/bms/capacity",
    "/sys/class/power_supply/battery_gauge/capacity",
    NULL
};

static const char *STATUS_CANDIDATES[] = {
    "/sys/class/power_supply/battery/status",
    "/sys/class/power_supply/Battery/status",
    "/sys/class/power_supply/bms/status",
    "/sys/class/power_supply/battery_gauge/status",
    "/sys/class/power_supply/qcom-battery/status",
    NULL
};

static const char *TEMP_CANDIDATES[] = {
    "/sys/class/power_supply/battery/temp",
    "/sys/class/power_supply/Battery/temp",
    "/sys/class/power_supply/bms/temp",
    "/sys/class/power_supply/battery_gauge/temp",
    "/sys/class/power_supply/bq27z561-0/temp",
    "/sys/class/power_supply/sc8545-standalone/temp",
    "/sys/class/power_supply/qcom-battery/temp",
    NULL
};

/**
 * 从给定的候选路径列表中尝试打开首个有效节点，并返回原生 POSIX 文件描述符。
 *
 * @param candidates 候选文件路径字符串数组（以 NULL 结尾）
 * @return 成功打开的文件描述符 fd (>= 0)，若均无法打开则返回 -1
 */
static int open_first_available(const char *candidates[]) {
    for (int i = 0; candidates[i] != NULL; ++i) {
        int fd = open(candidates[i], O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            LOGI("open_first_available: 成功打开节点 %s (fd=%d)", candidates[i], fd);
            return fd;
        }
    }
    return -1;
}

/**
 * 通过原子 pread 从文件偏移量 0 处极速读取数值，消除 seek 与用户态缓冲开销。
 *
 * @param fd 目标文件描述符
 * @return 解析得到的 long 整数，失败时返回 0
 */
static inline long read_long(int fd) {
    if (fd < 0) return 0;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) return 0;
    buffer[bytes_read] = '\0';
    return atol(buffer);
}

/**
 * 通过原子 pread 从文件偏移量 0 处极速读取数值。
 *
 * @param fd 目标文件描述符
 * @return 解析得到的 int 整数，失败时返回 0
 */
static inline int read_int(int fd) {
    if (fd < 0) return 0;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) return 0;
    buffer[bytes_read] = '\0';
    return atoi(buffer);
}

/**
 * 初始化底层节点文件描述符缓存。
 * 具备一次性探测保护：若此前已执行过初始化尝试，则直接返回上次结果，杜绝在 SELinux 限制环境下重复打开文件。
 *
 * @return 初始化结果，1 表示成功打开关键节点，0 表示打开失败
 */
static int init_file_cache() {
    if (g_cache.init_attempted) {
        return g_cache.initialized;
    }
    g_cache.init_attempted = 1;

    g_cache.voltage_fd = open_first_available(VOLTAGE_CANDIDATES);
    g_cache.current_fd = open_first_available(CURRENT_CANDIDATES);
    g_cache.capacity_fd = open_first_available(CAPACITY_CANDIDATES);
    g_cache.status_fd = open_first_available(STATUS_CANDIDATES);
    g_cache.temp_fd = open_first_available(TEMP_CANDIDATES);

    // 只要电流或电压至少有一个节点打开成功，即视为具备硬件直读能力
    if (g_cache.current_fd >= 0 || g_cache.voltage_fd >= 0) {
        g_cache.initialized = 1;
        LOGI("init_file_cache: 底层硬件节点 POSIX 文件描述符缓存初始化成功");
        return 1;
    }

    LOGE("init_file_cache: 无法直接打开电流或电压节点，可能受 SELinux 限制（已置位避免重试）");
    return 0;
}

extern "C" {

/**
 * 初始化 Linux 底层硬件电源节点的文件缓存。
 *
 * @param env JNI 运行环境指针
 * @param clazz Java 静态方法调用类对象
 * @return 1 表示成功，0 表示失败
 */
JNIEXPORT jint JNICALL
Java_com_battery_analysis_util_SysfsBatterySampler_nativeInit(
    JNIEnv *env,
    jclass clazz
) {
    return init_file_cache();
}

/**
 * 从底层节点直接读取当前瞬时电压（微伏 uV 或毫伏 mV）。
 *
 * @param env JNI 运行环境指针
 * @param clazz Java 静态方法调用类对象
 * @return 瞬时电压原始读数
 */
JNIEXPORT jlong JNICALL
Java_com_battery_analysis_util_SysfsBatterySampler_nativeGetVoltage(
    JNIEnv *env,
    jclass clazz
) {
    if (!g_cache.initialized && !init_file_cache()) {
        return 0;
    }
    return read_long(g_cache.voltage_fd);
}

/**
 * 从底层节点直接读取当前瞬时放电电流（微安 uA 或毫安 mA）。
 *
 * @param env JNI 运行环境指针
 * @param clazz Java 静态方法调用类对象
 * @return 瞬时电流原始读数（放电通常为负或绝对值）
 */
JNIEXPORT jlong JNICALL
Java_com_battery_analysis_util_SysfsBatterySampler_nativeGetCurrent(
    JNIEnv *env,
    jclass clazz
) {
    if (!g_cache.initialized && !init_file_cache()) {
        return 0;
    }
    return read_long(g_cache.current_fd);
}

/**
 * 从底层节点直接读取当前电池百分比。
 *
 * @param env JNI 运行环境指针
 * @param clazz Java 静态方法调用类对象
 * @return 电池剩余百分比 (0-100)
 */
JNIEXPORT jint JNICALL
Java_com_battery_analysis_util_SysfsBatterySampler_nativeGetCapacity(
    JNIEnv *env,
    jclass clazz
) {
    if (!g_cache.initialized && !init_file_cache()) {
        return 0;
    }
    return read_int(g_cache.capacity_fd);
}

/**
 * 从底层节点直接读取电池状态首字符（例如 'D' 表示 Discharging，'C' 表示 Charging）。
 *
 * @param env JNI 运行环境指针
 * @param clazz Java 静态方法调用类对象
 * @return 状态首字母 ASCII 码，若不可用返回 0
 */
JNIEXPORT jint JNICALL
Java_com_battery_analysis_util_SysfsBatterySampler_nativeGetStatus(
    JNIEnv *env,
    jclass clazz
) {
    if (!g_cache.initialized && !init_file_cache()) {
        return 0;
    }
    if (g_cache.status_fd < 0) return 0;

    char ch;
    ssize_t bytes_read = pread(g_cache.status_fd, &ch, 1, 0);
    return (bytes_read > 0) ? (jint)ch : 0;
}

/**
 * 从底层节点直接读取当前瞬时电池温度。
 *
 * @param env JNI 运行环境指针
 * @param clazz Java 静态方法调用类对象
 * @return 电池温度原始数值（通常为 0.1 摄氏度，如 370 表示 37.0℃）
 */
JNIEXPORT jint JNICALL
Java_com_battery_analysis_util_SysfsBatterySampler_nativeGetTemp(
    JNIEnv *env,
    jclass clazz
) {
    if (!g_cache.initialized && !init_file_cache()) {
        return 0;
    }
    return read_int(g_cache.temp_fd);
}

} // extern "C"
