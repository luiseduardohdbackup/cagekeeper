/* util.h
   Various utility functions

   Copyright (c) 2010-2012 Matthias Kramm <kramm@quiss.org> 
 
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef __util_h__
#define __util_h__

#include <stdbool.h>
#include <sys/select.h>
#undef assert // defined by sys/select.h

#ifdef __cplusplus
extern "C" {
#endif

#define DEBUG

extern void stdout_printf(const char*format, ...);
#ifdef DEBUG
#include <assert.h>
#define log_dbg         stdout_printf
#define log_msg         stdout_printf
#define log_warn        stdout_printf
#define log_err         stdout_printf
#else
#define assert(b)       do {} while(0)
#define log_dbg(f,...)  do {} while(0)
#define log_msg(f,...)  stdout_printf
#define log_warn(f,...) stdout_printf
#define log_err(f,...)  stdout_printf
#endif

void*memdup(const void*ptr, size_t size);
char*escape_string(const char*str);
char* allocprintf(const char*format, ...);
char* concat_paths(const char*base, const char*add);
void mkdir_p(const char*path);
char*read_file(const char*filename);

bool read_with_retry(int fd, void* data, int len);
bool read_with_timeout(int fd, void* data, int len, struct timeval* timeout);

#ifdef __cplusplus
}
#endif

#endif
