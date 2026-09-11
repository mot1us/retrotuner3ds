#ifndef TEST_THEME_STUB_3DS_H
#define TEST_THEME_STUB_3DS_H

/* The picker runs synchronously in this host test. Device lock behavior is
 * intentionally not emulated; these tests cover controls, persistence/layout. */
typedef int LightLock;
static inline void LightLock_Init(LightLock *lock) { *lock = 0; }
static inline void LightLock_Lock(LightLock *lock) { (void)lock; }
static inline void LightLock_Unlock(LightLock *lock) { (void)lock; }

#endif
