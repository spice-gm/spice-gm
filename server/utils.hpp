/*
   Copyright (C) 2019 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file utils.hpp
 * Generic utilities for C++
 */
#pragma once

#include <memory>
#include <atomic>

#include "push-visibility.h"

namespace red {


template <typename T>
inline T* add_ref(T* p)
{
    if (p) {
        p->ref();
    }
    return p;
}


/**
 * Smart pointer allocated once
 *
 * It just keep the pointer passed to constructor and delete
 * the object in the destructor. No copy or move allowed.
 * Very easy but make sure we don't change it and that's
 * initialized.
 */
template <typename T>
class unique_link
{
public:
    unique_link(): p(new T())
    {
    }
    unique_link(T* ptr): p(ptr)
    {
    }
    ~unique_link()
    {
        delete p;
    }
    T* operator->() noexcept
    {
        return p;
    }
    const T* operator->() const noexcept
    {
        return p;
    }
    T *get() const noexcept
    {
        return p;
    }
private:
    T *const p;
    unique_link(const unique_link&)=delete;
    void operator=(const unique_link&)=delete;
};


template <typename T>
struct GLibDeleter {
    void operator()(T* p)
    {
        g_free(p);
    }
};

/**
 * Allows to define a variable holding objects allocated with
 * g_malloc().
 *
 * @code{.cpp}
 * red::glib_unique_ptr<char> s = g_strdup("hello");
 * @endcode
 */
template <typename T>
using glib_unique_ptr = std::unique_ptr<T, GLibDeleter<T>>;


/**
 * @brief Returns the size of an array.
 * Introduced in C++17 but lacking in C++11
 */
template <class T, size_t N>
constexpr size_t size(const T (&array)[N]) noexcept
{
    return N;
}


template <typename T>
class weak_ptr;

/**
 * Basic shared pointer for internal reference
 *
 * Similar to STL version but using intrusive. This to allow creating multiple
 * shared pointers from the raw pointer without problems having the object
 * freed when one single set of shared pointer is removed. The code used
 * to increment the reference to make sure the object was not deleted in
 * some cases of self destruction.
 *
 * This class is inspired to boost::intrusive_ptr.
 *
 * To allow to reference and unrefered any object the object should
 * define shared_ptr_add_ref() and shared_ptr_unref(), both taking a pointer and incrementing and
 * decrementing respectively. You should not call these function yourselves.
 *
 * It's recommended that you create the object with red::make_shared() provided below. This to makes
 * sure that there is at least one strong reference to the object.
 */
template <typename T>
class shared_ptr
{
friend class weak_ptr<T>;
public:
    explicit shared_ptr(T *ptr=nullptr): p(ptr)
    {
        if (p) {
            shared_ptr_add_ref(p);
        }
    }
    template <class Q>
    explicit shared_ptr(Q *ptr): shared_ptr(static_cast<T*>(ptr))
    {
    }
    shared_ptr(const shared_ptr& rhs): p(rhs.p)
    {
        if (p) {
            shared_ptr_add_ref(p);
        }
    }
    template <class Q>
    shared_ptr(const shared_ptr<Q>& rhs): shared_ptr(static_cast<T*>(rhs.get()))
    {
    }
    shared_ptr& operator=(const shared_ptr& rhs)
    {
        if (rhs.p != p) {
            reset(rhs.p);
        }
        return *this;
    }
    template <class Q>
    shared_ptr& operator=(const shared_ptr<Q>& rhs)
    {
        reset(rhs.get());
        return *this;
    }
    shared_ptr(shared_ptr&& rhs): p(rhs.p)
    {
        rhs.p = nullptr;
    }
    shared_ptr& operator=(shared_ptr&& rhs)
    {
        if (p) {
            shared_ptr_unref(p);
        }
        p = rhs.p;
        rhs.p = nullptr;
        return *this;
    }
    ~shared_ptr()
    {
        if (p) {
            shared_ptr_unref(p);
        }
    }
    void reset(T *ptr=nullptr)
    {
        if (ptr) {
            shared_ptr_add_ref(ptr);
        }
        if (p) {
            shared_ptr_unref(p);
        }
        p = ptr;
    }
    operator bool() const
    {
        return p;
    }
    T& operator*() const noexcept
    {
        return *p;
    }
    T* operator->() const noexcept
    {
        return p;
    }
    T *get() const noexcept
    {
        return p;
    }
private:
    T* p;
    // for weak_ptr
    explicit shared_ptr(T *ptr, bool dummy): p(ptr)
    {
    }
};

template <class T, class O>
inline bool operator==(const shared_ptr<T>& a, const shared_ptr<O>& b)
{
    return a.get() == b.get();
}

template <class T, class O>
inline bool operator!=(const shared_ptr<T>& a, const shared_ptr<O>& b)
{
    return a.get() != b.get();
}

/**
 * Allows to create and object and wrap into a smart pointer at the same
 * time.
 * You should try to allocated any shared pointer managed object with this
 * function.
 */
template<typename T, typename... Args>
inline shared_ptr<T> make_shared(Args&&... args)
{
    return shared_ptr<T>(new T(args...));
}

/**
 * Utility to help implementing shared_ptr requirements.
 *
 * You should inherit publicly this class in order to have base internal reference counting
 * implementation.
 *
 * This class uses atomic operations and virtual destructor so it's not really light.
 * @see simple_ptr_counted
 */
class shared_ptr_counted
{
public:
    SPICE_CXX_GLIB_ALLOCATOR

    shared_ptr_counted(): ref_count(0)
    {
    }
protected:
    virtual ~shared_ptr_counted() {}
private:
    std::atomic_int ref_count;
    shared_ptr_counted(const shared_ptr_counted& rhs)=delete;
    void operator=(const shared_ptr_counted& rhs)=delete;
    friend inline void shared_ptr_add_ref(shared_ptr_counted*);
    friend inline void shared_ptr_unref(shared_ptr_counted*);
};

// implements requirements for shared_ptr
inline void shared_ptr_add_ref(shared_ptr_counted* p)
{
    ++p->ref_count;
}

inline void shared_ptr_unref(shared_ptr_counted* p)
{
    if (--p->ref_count == 0) {
        delete p;
    }
}

/**
 * Basic weak pointer for internal reference
 *
 * Similar to STL version like shared_ptr here.
 *
 * In order to support weak_ptr for an object weak_ptr_add_ref(),
 * weak_ptr_unref() and weak_ptr_lock() should be implemented. See below.
 */
template <typename T>
class weak_ptr
{
public:
    explicit weak_ptr(T *ptr=nullptr): p(ptr)
    {
        if (p) {
            weak_ptr_add_ref(p);
        }
    }
    weak_ptr(const weak_ptr& rhs): p(rhs.p)
    {
        if (p) {
            weak_ptr_add_ref(p);
        }
    }
    weak_ptr& operator=(const weak_ptr& rhs)
    {
        if (rhs.p != p) {
            reset(rhs.p);
        }
        return *this;
    }
    weak_ptr(weak_ptr&& rhs): p(rhs.p)
    {
        rhs.p = nullptr;
    }
    weak_ptr& operator=(weak_ptr&& rhs)
    {
        if (p) {
            weak_ptr_unref(p);
        }
        p = rhs.p;
        rhs.p = nullptr;
        return *this;
    }
    ~weak_ptr()
    {
        if (p) {
            weak_ptr_unref(p);
        }
    }
    // get a strong reference
    shared_ptr<T> lock()
    {
        return shared_ptr<T>(p && weak_ptr_lock(p) ? p : nullptr, false);
    }
    void reset(T *ptr=nullptr)
    {
        if (ptr) {
            weak_ptr_add_ref(ptr);
        }
        if (p) {
            weak_ptr_unref(p);
        }
        p = ptr;
    }
    // NOTE do not add operator bool using p, we need to check if still valid
private:
    T* p;
};


/**
 * Utility to help implementing shared ptr with weak semantic too
 *
 * Similar to shared_ptr_counted but you can use weak pointers too.
 */
class shared_ptr_counted_weak
{
public:
    SPICE_CXX_GLIB_ALLOCATOR

    shared_ptr_counted_weak(): ref_count(0), weak_count(1)
    {
    }
protected:
    virtual ~shared_ptr_counted_weak() {}
private:
    std::atomic_int ref_count;
    std::atomic_int weak_count;
    shared_ptr_counted_weak(const shared_ptr_counted_weak& rhs)=delete;
    void operator=(const shared_ptr_counted_weak& rhs)=delete;
    // this is used in order to use operator delete defined in this class, not global one
    void free_helper(void *p) { operator delete(p); }
    friend inline void shared_ptr_add_ref(shared_ptr_counted_weak*);
    friend inline void shared_ptr_unref(shared_ptr_counted_weak*);
    friend inline void weak_ptr_add_ref(shared_ptr_counted_weak*);
    friend inline void weak_ptr_unref(shared_ptr_counted_weak*);
    friend inline bool weak_ptr_lock(shared_ptr_counted_weak*);
};

// implements requirements for shared_ptr
inline void shared_ptr_add_ref(shared_ptr_counted_weak* p)
{
    ++p->ref_count;
}

inline void shared_ptr_unref(shared_ptr_counted_weak* p)
{
    if (--p->ref_count == 0) {
        p->~shared_ptr_counted_weak();
        std::atomic_thread_fence(std::memory_order_release);
        if (--p->weak_count == 0) {
            p->free_helper(p);
        }
    }
}

// implements requirements for weak_ptr
inline void weak_ptr_add_ref(shared_ptr_counted_weak* p)
{
    p->weak_count++;
}

inline void weak_ptr_unref(shared_ptr_counted_weak* p)
{
    if (--p->weak_count == 0) {
        std::atomic_thread_fence(std::memory_order_acquire);
        p->free_helper(p);
    }
}

inline bool weak_ptr_lock(shared_ptr_counted_weak* p)
{
    int count = (int) p->ref_count;
    do {
        if (count == 0) {
            return false;
        }
    } while (!p->ref_count.compare_exchange_weak(count, count + 1));
    return true;
}


} // namespace red

#include "pop-visibility.h"
