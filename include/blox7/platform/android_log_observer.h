#ifndef BLOX7_PLATFORM_ANDROID_LOG_OBSERVER_H_
#define BLOX7_PLATFORM_ANDROID_LOG_OBSERVER_H_

using Blox7AndroidLogObserver = void (*)(int priority, const char* tag,
                                            const char* message);

extern "C" void blox7_android_log_set_observer(
    Blox7AndroidLogObserver observer);

#endif  // BLOX7_PLATFORM_ANDROID_LOG_OBSERVER_H_
