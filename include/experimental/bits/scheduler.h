//
// scheduler.h
// ~~~~~~~~~~~
// Thread-safe scheduler implementation.
//
// Copyright (c) 2014 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef EXECUTORS_EXPERIMENTAL_BITS_SCHEDULER_H
#define EXECUTORS_EXPERIMENTAL_BITS_SCHEDULER_H

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>

#include <experimental/bits/call_stack.h>
#include <experimental/bits/operation.h>
#include <experimental/bits/small_block_recycler.h>

namespace std {
namespace experimental {

class __scheduler
{
public:
  struct _Context
  {
    __scheduler* _M_scheduler;
    __op_queue<__operation> _M_private_queue;
    typename __call_stack<__scheduler, _Context>::__context _M_context;
    unique_lock<mutex> _M_lock;

    explicit _Context(__scheduler* __s)
      : _M_scheduler(__s), _M_context(__s, *this), _M_lock(__s->_M_mutex)
    {
    }

    ~_Context()
    {
      if (!_M_private_queue._Empty())
      {
        if (!_M_lock.owns_lock())
          _M_lock.lock();

        _M_scheduler->_M_queue._Push(_M_private_queue);
      }
    }

    void _Lock()
    {
      if (!_M_lock.owns_lock())
        _M_lock.lock();

      if (!_M_private_queue._Empty())
        _M_scheduler->_M_queue._Push(_M_private_queue);
    }
  };

  typedef __call_stack<__scheduler, _Context> _Call_stack;

  __scheduler(size_t __concurrency_hint = ~size_t(0))
    : _M_outstanding_work(0), _M_stopped(false),
      _M_one_thread(__concurrency_hint == 1)
  {
  }

  template <class _F> void _Post(_F&& __f);
  template <class _F> void _Dispatch(_F&& __f);

  void _Work_started()
  {
    ++_M_outstanding_work;
  }

  void _Work_finished()
  {
    if (--_M_outstanding_work == 0)
      _Stop();
  }

  void _Stop()
  {
    lock_guard<mutex> __lock(_M_mutex);

    _M_stopped = true;
    _M_condition.notify_all();
  }

  bool _Stopped() const
  {
    lock_guard<mutex> __lock(_M_mutex);
    return _M_stopped;
  }

  void _Reset()
  {
    lock_guard<mutex> __lock(_M_mutex);
    _M_stopped = false;
  }

  size_t _Run()
  {
    if (_M_outstanding_work == 0)
    {
      _Stop();
      return 0;
    }

    _Context __ctx(this);

    std::size_t __n = 0;
    for (; _Do_run_one(__ctx._M_lock); __ctx._Lock())
      if (__n != (numeric_limits<size_t>::max)())
        ++__n;
    return __n;
  }

  size_t _Run_one()
  {
    if (_M_outstanding_work == 0)
    {
      _Stop();
      return 0;
    }

    _Context __ctx(this);

    return _Do_run_one(__ctx._M_lock);
  }

  template <class _Rep, class _Period>
  size_t _Run_for(const chrono::duration<_Rep, _Period>& __rel_time)
  {
    return this->_Run_until(chrono::steady_clock::now() + __rel_time);
  }

  template <class _Clock, class _Duration>
  size_t _Run_until(const chrono::time_point<_Clock, _Duration>& __abs_time)
  {
    if (_M_outstanding_work == 0)
    {
      _Stop();
      return 0;
    }

    _Context __ctx(this);

    std::size_t __n = 0;
    for (; _Do_run_one_until(__ctx._M_lock, __abs_time); __ctx._Lock())
      if (__n != (numeric_limits<size_t>::max)())
        ++__n;
    return __n;
  }

  size_t _Poll()
  {
    if (_M_outstanding_work == 0)
    {
      _Stop();
      return 0;
    }

    _Context __ctx(this);

    std::size_t __n = 0;
    for (; _Do_poll_one(__ctx._M_lock); __ctx._Lock())
      if (__n != (numeric_limits<size_t>::max)())
        ++__n;
    return __n;
  }

  size_t _Poll_one()
  {
    if (_M_outstanding_work == 0)
    {
      _Stop();
      return 0;
    }

    _Context __ctx(this);

    return _Do_poll_one(__ctx._M_lock);
  }

private:
  size_t _Do_run_one(unique_lock<mutex>& __lock)
  {
    while (_M_queue._Empty() && !_M_stopped)
      _M_condition.wait(__lock);

    if (_M_stopped)
      return 0;

    __operation* __op = _M_queue._Front();
    _M_queue._Pop();

    if (!_M_one_thread && !_M_queue._Empty())
      _M_condition.notify_one();

    __lock.unlock();

    __op->_Complete();
    return 1;
  }

  template <class _Clock, class _Duration>
  size_t _Do_run_one_until(unique_lock<mutex>& __lock,
    const chrono::time_point<_Clock, _Duration>& __abs_time)
  {
    if (_Clock::now() >= __abs_time)
      return 0;

    while (_M_queue._Empty() && !_M_stopped)
      if (_M_condition.wait_until(__lock, __abs_time) == cv_status::timeout)
        return 0;

    if (_M_stopped)
      return 0;

    __operation* __op = _M_queue._Front();
    _M_queue._Pop();

    if (!_M_one_thread && !_M_queue._Empty())
      _M_condition.notify_one();

    __lock.unlock();

    __op->_Complete();
    return 1;
  }

  size_t _Do_poll_one(unique_lock<mutex>& __lock)
  {
    if (_M_queue._Empty() || _M_stopped)
      return 0;

    __operation* __op = _M_queue._Front();
    _M_queue._Pop();

    if (!_M_one_thread && !_M_queue._Empty())
      _M_condition.notify_one();

    __lock.unlock();

    __op->_Complete();
    return 1;
  }

  mutable mutex _M_mutex;
  condition_variable _M_condition;
  __op_queue<__operation> _M_queue;
  atomic<size_t> _M_outstanding_work;
  bool _M_stopped;
  const bool _M_one_thread;
};

template <class _Func>
class __scheduler_op
  : public __operation
{
public:
  __scheduler_op(const __scheduler_op&) = delete;
  __scheduler_op& operator=(const __scheduler_op&) = delete;

  template <class _F> __scheduler_op(_F&& __f, __scheduler& __s)
    : _M_func(forward<_F>(__f)), _M_owner(&__s)
  {
    _M_owner->_Work_started();
  }

  __scheduler_op(__scheduler_op&& __s)
    : _M_func(std::move(__s._M_func)), _M_owner(__s._M_owner)
  {
    __s._M_owner = 0;
  }

  ~__scheduler_op()
  {
    if (_M_owner)
      _M_owner->_Work_finished();
  }

  virtual void _Complete()
  {
    __small_block_recycler<>::_Unique_ptr<__scheduler_op> __op(this);
    __scheduler_op __tmp(std::move(*this));
    __op.reset();
    __tmp._M_func();
  }

  virtual void _Destroy()
  {
    __small_block_recycler<>::_Destroy(this);
  }

private:
  _Func _M_func;
  __scheduler* _M_owner;
};

template <class _F> void __scheduler::_Post(_F&& __f)
{
  typedef typename decay<_F>::type _Func;
  __small_block_recycler<>::_Unique_ptr<__scheduler_op<_Func>> __op(
    __small_block_recycler<>::_Create<__scheduler_op<_Func>>(forward<_F>(__f), *this));

  if (_Context* __ctx = _M_one_thread ? _Call_stack::_Contains(this) : nullptr)
  {
    __ctx->_M_private_queue._Push(__op.get());
  }
  else
  {
    lock_guard<mutex> lock(_M_mutex);

    _M_queue._Push(__op.get());
    if (_M_queue._Front() == __op.get())
      _M_condition.notify_one();
  }

  __op.release();
}

template <class _F> void __scheduler::_Dispatch(_F&& __f)
{
  typedef typename decay<_F>::type _Func;
  if (_Call_stack::_Contains(this))
  {
    _Func __tmp(forward<_F>(__f));
    __tmp();
  }
  else
  {
    this->_Post(forward<_F>(__f));
  }
}

} // namespace experimental
} // namespace std

#endif
