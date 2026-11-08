// https://github.com/cctbx/cctbx_project/blob/master/boost_adaptbx/floating_point_exceptions.h
#ifndef FLOATING_POINT_EXCEPTIONS_H
#define FLOATING_POINT_EXCEPTIONS_H

#include <atomic>

#ifdef __GNUC__
#include <fenv.h>
#ifdef __SSE2__
#include <xmmintrin.h>
#endif
#endif


namespace boost_adaptbx { namespace floating_point {

  /** Floating point exception trapping

      Background information:
      gcc on x86-64 (all platforms) [2],
      Apple gcc on Intel macs [1]:
        floating-point math is done on vector unit (SSE2) by default

      gcc on i386 (all platform except Apple) [2]:
        floating-point math is done on scalar unit (i387)

      Linux:
        feenableexcept enables/disables FP exception trapping for i387 and SSE2
      gcc on all Intel platform:
        _MM_SET_EXCEPTION_MASK from xmmintrin.h enables/disables exception
        trapping for SSE2

      [1] http://developer.apple.com/documentation/performance/Conceptual/Accelerate_sse_migration/migration_sse_translation/chapter_4_section_2.html
      [2] GCC documentation
  */

  /** Set once a software GL renderer or a CPU Vulkan device is in use, and never cleared outside tests.
      Mesa's software drivers raise these exceptions inside their own shader compilers on whichever thread
      drives them, and threads inherit the float environment of the thread that creates them, so while it
      is set no thread of a debug build arms the traps. */
  inline std::atomic<bool>& software_renderer_present()
  {
      static std::atomic<bool> present { false };
      return present;
  }

  inline void trap_exceptions(bool division_by_zero, bool invalid, bool overflow) {
#ifdef DEBUG
      if (software_renderer_present().load(std::memory_order_relaxed)) {
          division_by_zero = invalid = overflow = false;
      }
#endif
#ifdef __linux
      int enabled = 0;
      int disabled = 0;
#ifdef FE_DIVBYZERO
      if (division_by_zero)
          enabled |= FE_DIVBYZERO;
      else
          disabled |= FE_DIVBYZERO;
#endif
#ifdef FE_INVALID
      if (invalid)
          enabled |= FE_INVALID;
      else
          disabled |= FE_INVALID;
#endif
#ifdef FE_OVERFLOW
      if (overflow)
          enabled |= FE_OVERFLOW;
      else
          disabled |= FE_OVERFLOW;
#endif
      fedisableexcept(disabled);
      // A flag raised while its trap was off would otherwise fire on the next float instruction.
      feclearexcept(enabled);
      feenableexcept(enabled);
#elif defined(__APPLE_CC__) && defined(__SSE2__)
#if 1
      unsigned int mask
        = _MM_MASK_INEXACT | _MM_MASK_UNDERFLOW | _MM_MASK_DENORM;
      if (!division_by_zero) mask |= _MM_MASK_DIV_ZERO;
      if (!invalid) mask |= _MM_MASK_INVALID;
      if (!overflow) mask |= _MM_MASK_OVERFLOW;
#else
      unsigned int mask = _MM_GET_EXCEPTION_MASK();
      if (division_by_zero) mask &= ~_MM_MASK_DIV_ZERO;
      if (invalid) mask &= ~_MM_MASK_INVALID;
      if (overflow) mask &= ~_MM_MASK_OVERFLOW;
#endif
      _MM_SET_EXCEPTION_MASK(mask);
    #endif
  }

  inline bool is_division_by_zero_trapped() {
    #ifdef __linux
      #ifdef FE_DIVBYZERO
        return fegetexcept() & FE_DIVBYZERO;
      #else
        return false;
      #endif
    #elif defined(__APPLE_CC__) && defined(__SSE2__)
      return !(_MM_GET_EXCEPTION_MASK() & _MM_MASK_DIV_ZERO);
    #else
      return false;
    #endif
  }

  inline bool is_invalid_trapped() {
    #ifdef __linux
      #ifdef FE_INVALID
        return fegetexcept() & FE_INVALID;
      #else
        return false;
      #endif
    #elif defined(__APPLE_CC__) && defined(__SSE2__)
      return !(_MM_GET_EXCEPTION_MASK() & _MM_MASK_INVALID);
    #else
      return false;
    #endif
  }

  inline bool is_overflow_trapped() {
    #ifdef __linux
      #ifdef FE_OVERFLOW
        return fegetexcept() & FE_OVERFLOW;
      #else
        return false;
      #endif
    #elif defined(__APPLE_CC__) && defined(__SSE2__)
      return !(_MM_GET_EXCEPTION_MASK() & _MM_MASK_OVERFLOW);
    #else
      return false;
    #endif
  }

  class exception_trapping
  {
    private:
      bool division_by_zero_on_entry, invalid_on_entry, overflow_on_entry;
      bool division_by_zero_, invalid_, overflow_;

    public:
      enum { dont_trap=0, division_by_zero=1, invalid=2, overflow=4 };

      exception_trapping(int flag)
        : division_by_zero_on_entry(is_division_by_zero_trapped()),
          invalid_on_entry(is_invalid_trapped()),
          overflow_on_entry(is_overflow_trapped()),
          division_by_zero_(flag & division_by_zero),
          invalid_(flag & invalid),
          overflow_(flag & overflow)
      {
        trap_exceptions(division_by_zero_, invalid_, overflow_);
      }

      ~exception_trapping() {
        trap_exceptions(division_by_zero_on_entry,
                        invalid_on_entry,
                        overflow_on_entry);
      }
  };

  /** Turns trapping off for the calling thread in a debug build; does nothing in a release build. For a thread
      that drives a software driver before the process knows whether it keeps it. A scoped guard alive on the
      thread restores the traps when it ends, so call it outside any. */
  inline void disarm_trapping_for_thread()
  {
#ifdef DEBUG
      trap_exceptions(false, false, false);
#endif
  }

  /** Sets software_renderer_present() and, in a debug build, turns trapping off for the calling thread. */
  inline void note_software_renderer()
  {
      software_renderer_present().store(true);
#ifdef DEBUG
      trap_exceptions(false, false, false);
#endif
  }

  /** For a long-lived thread that armed its traps once at start-up: turns them off in a debug build if
      software_renderer_present() was set since. */
  inline void follow_software_renderer()
  {
#ifdef DEBUG
      if (software_renderer_present().load(std::memory_order_relaxed)) {
          trap_exceptions(false, false, false);
      }
#endif
  }

  /** Keeps trapping off for the calling thread while it lives in a debug build; does nothing in a
      release build. Third-party maths (OpenColorIO LUT inversion, OIIO colour probing) legitimately
      raises these exceptions, and render threads trap them in debug builds. */
  class FpTrapGuard {
  public:
      FpTrapGuard()
#ifdef DEBUG
          : trap_(exception_trapping::dont_trap)
#endif
      {
      }

      FpTrapGuard(const FpTrapGuard&) = delete;
      FpTrapGuard& operator=(const FpTrapGuard&) = delete;

  private:
#ifdef DEBUG
      exception_trapping trap_;
#endif
  };
}} // boost_adaptbx::floating_point

#endif // GUARD
