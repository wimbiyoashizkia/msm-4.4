#include "linux/kallsyms.h"

#define RE_EXPORT_SYMBOL1(ret, func, t1, v1)                                   \
	ret ksu_##func(t1 v1)                                                  \
	{                                                                      \
		return func(v1);                                               \
	}                                                                      \
	EXPORT_SYMBOL(ksu_##func);

#define RE_EXPORT_SYMBOL2(ret, func, t1, v1, t2, v2)                           \
	ret ksu_##func(t1 v1, t2 v2)                                           \
	{                                                                      \
		return func(v1, v2);                                           \
	}                                                                      \
	EXPORT_SYMBOL(ksu_##func);

RE_EXPORT_SYMBOL1(unsigned long, kallsyms_lookup_name, const char *, name)