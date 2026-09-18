/*
 * The development watchdog (§3.15). Its entry point is osoe_sweep_expired,
 * declared in base/sweep.h, which every library wait calls when a safety-sweep
 * deadline expires; this header carries only the build-time default.
 *
 * On in the debug, asan and tsan presets and off in release, because production
 * relies on the launcher tearing the job down, not on this. OSOE_WATCHDOG=0
 * or 1 in the environment overrides the default either way.
 */
#ifndef OSOE_WATCHDOG_H
#define OSOE_WATCHDOG_H

#ifndef OSOE_WATCHDOG_DEFAULT
#define OSOE_WATCHDOG_DEFAULT 1
#endif

#endif /* OSOE_WATCHDOG_H */
