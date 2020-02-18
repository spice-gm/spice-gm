/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2017 Red Hat, Inc.

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
#ifndef TEST_GLIB_COMPAT_H_
#define TEST_GLIB_COMPAT_H_

#include <glib.h>

/* this avoids deprecation warning */
static inline void
g_test_expect_message_no_warnings(const gchar *log_domain, GLogLevelFlags log_level,
                                  const gchar *pattern)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_test_expect_message(log_domain, log_level, pattern);
    G_GNUC_END_IGNORE_DEPRECATIONS
}
static inline void
g_test_assert_expected_messages_internal_no_warnings(const char *domain,
                                                     const char *file, int line, const char *func)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_test_assert_expected_messages_internal(domain, file, line, func);
    G_GNUC_END_IGNORE_DEPRECATIONS
}
#define g_test_expect_message g_test_expect_message_no_warnings
#define g_test_assert_expected_messages_internal g_test_assert_expected_messages_internal_no_warnings

/* GLIB_CHECK_VERSION(2, 40, 0) */
#ifndef g_assert_nonnull
#define g_assert_nonnull g_assert
#endif

/* Added in glib 2.50 */
#ifndef G_PID_FORMAT
#ifndef G_OS_WIN32
#define G_PID_FORMAT "i"
#else
#define G_PID_FORMAT "p"
#endif /* G_OS_WIN32 */
#endif /* G_PID_FORMAT */

#endif // TEST_GLIB_COMPAT_H_
