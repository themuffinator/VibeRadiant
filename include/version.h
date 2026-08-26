// Meson links build-time version data from a small generated library. The
// legacy Make build supplies complete version macros instead.
#ifdef VIBERADIANT_USE_VCS_VERSION
extern const char RADIANT_VERSION[];
extern const char Q3MAP_VERSION[];
extern const char RADIANT_BUILD_DATE[];
extern const char RADIANT_BUILD_TIME[];
#else
#ifndef RADIANT_VERSION
#error no RADIANT_VERSION defined
#endif
#define RADIANT_BUILD_DATE __DATE__
#define RADIANT_BUILD_TIME __TIME__
#endif
#ifndef RADIANT_MAJOR_VERSION
#error no RADIANT_MAJOR_VERSION defined
#endif
#ifndef RADIANT_MINOR_VERSION
#error no RADIANT_MINOR_VERSION defined
#endif
