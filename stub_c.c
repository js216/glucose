/* Link-time stub for the device's bionic libc.so (see stub_android.c).
 * Add symbols here as the native code starts using them.
 */
typedef unsigned long size_t;
void *memset(void *d, int c, size_t n)          { (void)c; (void)n; return d; }
void *memcpy(void *d, const void *s, size_t n)  { (void)s; (void)n; return d; }
int   memcmp(const void *a, const void *b, size_t n) { (void)a; (void)b; (void)n; return 0; }
int   snprintf(char *s, size_t n, const char *f, ...) { (void)s; (void)n; (void)f; return 0; }
int   strcmp(const char *a, const char *b)      { (void)a; (void)b; return 0; }
int   strncmp(const char *a, const char *b, size_t n) { (void)a; (void)b; (void)n; return 0; }
int   clock_gettime(int clk, void *ts)          { (void)clk; (void)ts; return 0; }
void *malloc(size_t n)                          { (void)n; return 0; }
void *calloc(size_t a, size_t b)                { (void)a; (void)b; return 0; }
void  free(void *p)                             { (void)p; }
int   open(const char *p, int f, ...)           { (void)p; (void)f; return -1; }
long  read(int fd, void *b, size_t n)           { (void)fd; (void)b; (void)n; return -1; }
long  write(int fd, const void *b, size_t n)    { (void)fd; (void)b; (void)n; return -1; }
int   close(int fd)                             { (void)fd; return 0; }
int   unlink(const char *p)                     { (void)p; return 0; }
long  lseek(int fd, long off, int w)            { (void)fd; (void)off; (void)w; return 0; }
int   timerfd_create(int c, int f)              { (void)c; (void)f; return -1; }
int   timerfd_settime(int fd, int f, const void *n, void *o) { (void)fd; (void)f; (void)n; (void)o; return 0; }
