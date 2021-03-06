#include <time.h>
#include <fcntl.h>
#include "ruby.h"
#include "ruby/io.h"
#include "polyphony.h"
#include "backend_common.h"

inline void backend_base_initialize(struct Backend_base *base) {
  runqueue_initialize(&base->runqueue);
  base->currently_polling = 0;
  base->pending_count = 0;
  base->idle_gc_period = 0;
  base->idle_gc_last_time = 0;
  base->idle_proc = Qnil;
  base->trace_proc = Qnil;
}

inline void backend_base_finalize(struct Backend_base *base) {
  runqueue_finalize(&base->runqueue);
}

inline void backend_base_mark(struct Backend_base *base) {
  if (base->idle_proc != Qnil) rb_gc_mark(base->idle_proc);
  if (base->trace_proc != Qnil) rb_gc_mark(base->trace_proc);
  runqueue_mark(&base->runqueue);
}

VALUE backend_base_switch_fiber(VALUE backend, struct Backend_base *base) {
  VALUE current_fiber = rb_fiber_current();
  runqueue_entry next;
  unsigned int pending_ops_count = base->pending_count;
  unsigned int backend_was_polled = 0;
  unsigned int idle_tasks_run_count = 0;

  if (SHOULD_TRACE(base) && (rb_ivar_get(current_fiber, ID_ivar_running) != Qfalse))
    TRACE(base, 2, SYM_fiber_switchpoint, current_fiber);

  while (1) {
    next = runqueue_shift(&base->runqueue);
    if (next.fiber != Qnil) {
      // Polling for I/O op completion is normally done when the run queue is
      // empty, but if the runqueue never empties, we'll never get to process
      // any event completions. In order to prevent this, an anti-starvation
      // mechanism is employed, under the following conditions:
      // - a blocking poll was not yet performed
      // - there are pending blocking operations
      // - the runqueue shift count has reached a fixed threshold (currently 64), or
      // - the next fiber is the same as the current fiber (a single fiber is snoozing)
      int do_nonblocking_poll = !backend_was_polled && pending_ops_count && (
        runqueue_should_poll_nonblocking(&base->runqueue) || next.fiber == current_fiber
      );
      if (do_nonblocking_poll) Backend_poll(backend, Qnil);

      break;
    }
    
    if (!idle_tasks_run_count) {
      idle_tasks_run_count++;
      backend_run_idle_tasks(base);
    }
    if (pending_ops_count == 0) break;
    Backend_poll(backend, Qtrue);
    backend_was_polled = 1;
  }

  if (next.fiber == Qnil) return Qnil;

  // run next fiber
  COND_TRACE(base, 3, SYM_fiber_run, next.fiber, next.value);

  rb_ivar_set(next.fiber, ID_ivar_runnable, Qnil);
  RB_GC_GUARD(next.fiber);
  RB_GC_GUARD(next.value);
  return (next.fiber == current_fiber) ?
    next.value : FIBER_TRANSFER(next.fiber, next.value);
}

void backend_base_schedule_fiber(VALUE thread, VALUE backend, struct Backend_base *base, VALUE fiber, VALUE value, int prioritize) {
  int already_runnable;

  if (rb_fiber_alive_p(fiber) != Qtrue) return;
  already_runnable = rb_ivar_get(fiber, ID_ivar_runnable) != Qnil;

  COND_TRACE(base, 3, SYM_fiber_schedule, fiber, value);
  (prioritize ? runqueue_unshift : runqueue_push)(&base->runqueue, fiber, value, already_runnable);
  if (!already_runnable) {
    rb_ivar_set(fiber, ID_ivar_runnable, Qtrue);
    if (rb_thread_current() != thread) {
      // If the fiber scheduling is done across threads, we need to make sure the
      // target thread is woken up in case it is in the middle of running its
      // event selector. Otherwise it's gonna be stuck waiting for an event to
      // happen, not knowing that it there's already a fiber ready to run in its
      // run queue.
      Backend_wakeup(backend);
    }
  }
}


inline void backend_trace(struct Backend_base *base, int argc, VALUE *argv) {
  if (base->trace_proc == Qnil) return;

  rb_funcallv(base->trace_proc, ID_call, argc, argv);
}

#ifdef POLYPHONY_USE_PIDFD_OPEN
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434   /* System call # on most architectures */
#endif

inline int pidfd_open(pid_t pid, unsigned int flags) {
  return syscall(__NR_pidfd_open, pid, flags);
}
#endif

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
// the following is copied verbatim from the Ruby source code (io.c)

inline int io_setstrbuf(VALUE *str, long len) {
  #ifdef _WIN32
    len = (len + 1) & ~1L;	/* round up for wide char */
  #endif
  if (*str == Qnil) {
    *str = rb_str_new(0, len);
    return 1;
  }
  else {
    VALUE s = StringValue(*str);
    long clen = RSTRING_LEN(s);
    if (clen >= len) {
      rb_str_modify(s);
      return 0;
    }
    len -= clen;
  }
  rb_str_modify_expand(*str, len);
  return 0;
}

#define MAX_REALLOC_GAP 4096

inline void io_shrink_read_string(VALUE str, long n) {
  if (rb_str_capacity(str) - n > MAX_REALLOC_GAP) {
    rb_str_resize(str, n);
  }
}

inline void io_set_read_length(VALUE str, long n, int shrinkable) {
  if (RSTRING_LEN(str) != n) {
    rb_str_modify(str);
    rb_str_set_len(str, n);
    if (shrinkable) io_shrink_read_string(str, n);
  }
}

inline rb_encoding* io_read_encoding(rb_io_t *fptr) {
  if (fptr->encs.enc) {
	  return fptr->encs.enc;
  }
  return rb_default_external_encoding();
}

inline VALUE io_enc_str(VALUE str, rb_io_t *fptr) {
  OBJ_TAINT(str);
  rb_enc_associate(str, io_read_encoding(fptr));
  return str;
}

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

inline VALUE backend_await(struct Backend_base *backend) {
  VALUE ret;
  backend->pending_count++;
  ret = Thread_switch_fiber(rb_thread_current());
  backend->pending_count--;
  RB_GC_GUARD(ret);
  return ret;
}

inline VALUE backend_snooze() {
  VALUE ret;
  Fiber_make_runnable(rb_fiber_current(), Qnil);
  ret = Thread_switch_fiber(rb_thread_current());
  return ret;
}

inline void rectify_io_file_pos(rb_io_t *fptr) {
  // Apparently after reopening a closed file, the file position is not reset,
  // which causes the read to fail. Fortunately we can use fptr->rbuf.len to
  // find out if that's the case.
  // See: https://github.com/digital-fabric/polyphony/issues/30
  if (fptr->rbuf.len > 0) {
    lseek(fptr->fd, -fptr->rbuf.len, SEEK_CUR);
    fptr->rbuf.len = 0;
  }
}

inline double current_time() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  long long ns = ts.tv_sec;
  ns = ns * 1e9 + ts.tv_nsec;
  double t = ns;
  return t / 1e9;
}

inline VALUE backend_timeout_exception(VALUE exception) {
  if (rb_obj_is_kind_of(exception, rb_cArray) == Qtrue)
    return rb_funcall(rb_ary_entry(exception, 0), ID_new, 1, rb_ary_entry(exception, 1));
  else if (rb_obj_is_kind_of(exception, rb_cClass) == Qtrue)
    return rb_funcall(exception, ID_new, 0);
  else
    return rb_funcall(rb_eRuntimeError, ID_new, 1, exception);
}

VALUE Backend_timeout_safe(VALUE arg) {
  return rb_yield(arg);
}

VALUE Backend_timeout_rescue(VALUE arg, VALUE exception) {
  return exception;
}

VALUE Backend_timeout_ensure_safe(VALUE arg) {
  return rb_rescue2(Backend_timeout_safe, Qnil, Backend_timeout_rescue, Qnil, rb_eException, (VALUE)0);
}

static VALUE empty_string = Qnil;

VALUE Backend_sendv(VALUE self, VALUE io, VALUE ary, VALUE flags) {
  switch (RARRAY_LEN(ary)) {
  case 0:
    return Qnil;
  case 1:
    return Backend_send(self, io, RARRAY_AREF(ary, 0), flags);
  default:
    if (empty_string == Qnil) {
      empty_string = rb_str_new_literal("");
      rb_global_variable(&empty_string);
    }
    VALUE joined = rb_ary_join(ary, empty_string);
    VALUE result = Backend_send(self, io, joined, flags);
    RB_GC_GUARD(joined);
    return result;
  }
}

inline void io_verify_blocking_mode(rb_io_t *fptr, VALUE io, VALUE blocking) {
  VALUE blocking_mode = rb_ivar_get(io, ID_ivar_blocking_mode);
  if (blocking == blocking_mode) return;

  rb_ivar_set(io, ID_ivar_blocking_mode, blocking);

#ifdef _WIN32
  if (blocking != Qtrue)
    rb_w32_set_nonblock(fptr->fd);
#elif defined(F_GETFL)
  int flags = fcntl(fptr->fd, F_GETFL);
  if (flags == -1) return;
  int is_nonblocking = flags & O_NONBLOCK;
  
  if (blocking == Qtrue) {
    if (!is_nonblocking) return;
    flags &= ~O_NONBLOCK;
  } else {
    if (is_nonblocking) return;
    flags |= O_NONBLOCK;
  }
  fcntl(fptr->fd, F_SETFL, flags);
#endif
}

inline void backend_run_idle_tasks(struct Backend_base *base) {
  if (base->idle_proc != Qnil)
    rb_funcall(base->idle_proc, ID_call, 0);

  if (base->idle_gc_period == 0) return;

  double now = current_time();
  if (now - base->idle_gc_last_time < base->idle_gc_period) return;

  base->idle_gc_last_time = now;
  rb_gc_enable();
  rb_gc_start();
  rb_gc_disable();
}
