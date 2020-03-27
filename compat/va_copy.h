//
// Created by chen zhenhui on 2020/3/27.
//

#ifndef COMPAT_VA_COPY_H
#define COMPAT_VA_COPY_H

#include <stdarg.h>

#if !defined(va_copy) && defined(_MSC_VER)
#define va_copy(dst, src) ((dst) = (src))
#endif
#if !defined(va_copy) && defined(__GNUC__) && __GNUC__ < 3
#define va_copy(dst, src) __va_copy(dst, src)
#endif

#endif /* COMPAT_VA_COPY_H */