/* Link-time stub for the device's liblog.so (see stub_android.c). */
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    (void)prio; (void)tag; (void)fmt;
    return 0;
}
