#pragma once

#define TZF_VER_MAJOR     1
#define TZF_VER_MINOR     8
#define TZF_VER_BUILD     6
#define TZF_VER_REV       0
#define TZF_VER_SUFFIX_1  "-TS"

#if defined(TZF_RELEASE_BUILD)
#define TZF_VER_SUFFIX_2  ""
#else
#define TZF_VER_SUFFIX_2  " (WIP)"
#endif

#define _A2(a)     #a
#define  _A(a)  _A2(a)
#define _L2(w)  L ## w
#define  _L(w) _L2(w)

#if (TZF_REV > 0)
#define TZF_VERSION_STR_A    _A(TZF_VER_MAJOR) "." _A(TZF_VER_MINOR) "." _A(TZF_VER_BUILD) "." _A(TZF_VER_REV) TZF_VER_SUFFIX_1  TZF_VER_SUFFIX_2
#else
#define TZF_VERSION_STR_A    _A(TZF_VER_MAJOR) "." _A(TZF_VER_MINOR) "." _A(TZF_VER_BUILD) TZF_VER_SUFFIX_1  TZF_VER_SUFFIX_2
#endif

#define TZF_VERSION_STR_W _L(TZF_VERSION_STR_A)

#define TZF_FILE_VERSION     TZF_VER_MAJOR,TZF_VER_MINOR,TZF_VER_BUILD,TZF_VER_REV
#define TZF_PRODUCT_VERSION  TZF_VER_MAJOR,TZF_VER_MINOR,TZF_VER_BUILD,TZF_VER_REV
