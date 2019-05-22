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

/* Generic utilities for C++
 */
#pragma once

#include <memory>

namespace red {


template <typename T>
inline T* add_ref(T* p)
{
    if (p) {
        p->ref();
    }
    return p;
}


/* Smart pointer allocated once
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
    unique_link(T* p): p(p)
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

template <typename T>
using glib_unique_ptr = std::unique_ptr<T, GLibDeleter<T>>;


/* Returns the size of an array.
 * Introduced in C++17 but lacking in C++11
 */
template <class T, size_t N>
constexpr size_t size(const T (&array)[N]) noexcept
{
    return N;
}


} // namespace red
