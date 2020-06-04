/*
   Copyright (C) 2020 Red Hat, Inc.

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

/* Implementation of a list with more "safe" iterators.
 * Specifically the item under an iterator can be removed while scanning
 * the list. This to allow objects in the list to delete themselves from
 * the list.
 */
#pragma once

#include <memory>
#include <forward_list>
#include <algorithm>
#include <glib.h>

#include "push-visibility.h"

namespace red {

template <class T>
struct Mallocator
{
    typedef T value_type;
    Mallocator() = default;
    template <class U>
    constexpr Mallocator(const Mallocator<U>&) noexcept
    {
    }
    T* allocate(size_t n)
    {
        return static_cast<T*>(g_malloc_n(n, sizeof(T)));
    }
    void deallocate(T* p, size_t) noexcept
    {
        g_free(p);
    }
};

template <class T, class U>
bool operator==(const Mallocator<T>&, const Mallocator<U>&)
{
    return true;
}

template <class T, class U>
bool operator!=(const Mallocator<T>&, const Mallocator<U>&)
{
    return false;
}


template <typename T>
class safe_list
{
    typedef typename std::forward_list<T> wrapped;
    typename std::forward_list<T,Mallocator<T>> list;
    class iterator;
public:
    void push_front(const T& v)
    {
        list.push_front(v);
    }
    void remove(const T& v)
    {
        list.remove(v);
    }
    void clear()
    {
        list.clear();
    }
    void pop_front()
    {
        list.pop_front();
    }
    iterator begin() noexcept
    {
        return iterator(list.begin());
    }
    iterator end() noexcept
    {
        return iterator(list.end());
    }
    size_t size()
    {
        return std::distance(begin(), end());
    }
    bool empty() const
    {
        return list.empty();
    }
};

template <typename T>
class safe_list<T>::iterator: public std::iterator<std::forward_iterator_tag, T>
{
    typedef typename std::forward_list<T,Mallocator<T>>::iterator wrapped;
    wrapped curr, next;
public:
    iterator(wrapped init_curr) :
        curr(init_curr),
        next(init_curr != wrapped() ? ++init_curr : wrapped())
    {
    }
    iterator& operator++()
    {
        curr = next;
        if (next != wrapped()) {
            ++next;
        }
        return *this;
    }
    iterator operator++(int)
    {
        iterator tmp(*this);
        operator++();
        return tmp;
    }
    bool operator==(const iterator& rhs) const
    {
        return curr == rhs.curr;
    }
    bool operator!=(const iterator& rhs) const
    {
        return curr != rhs.curr;
    }
    T& operator*()
    {
        return *curr;
    }
};

} // namespace red

#include "pop-visibility.h"
