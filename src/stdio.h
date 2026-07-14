/* Empty shim. The JDK's jni.h includes <stdio.h> out of tradition but uses
 * nothing from it; the real glibc header won't compile in a freestanding
 * Android-target build. va_list comes from clang's own <stdarg.h>.
 */
