// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_PORT_PORT_H_
#define STORAGE_LEVELDB_PORT_PORT_H_

#include <string.h>
// This directory contains interfaces and implementations that isolate the
// rest of the package from platform details.
// 此目录包含接口和实现，用于隔离包的其余部分与平台细节。

// Code in the rest of the package includes "port.h" from this directory.
// "port.h" in turn includes a platform specific "port_<platform>.h" file
// that provides the platform specific implementation.
// 包中的代码包括此目录中的“port.h”。“port.h”又包括一个特定于平台的“port_<platform>.h”文件，
// 该文件提供特定于平台的实现。

// See port_stdcxx.h for an example of what must be provided in a platform
// specific header file.
// 请参阅port_stdcxx.h，了解平台特定头文件中必须提供的内容示例。


// Include the appropriate platform specific file below.  If you are
// porting to a new platform, see "port_example.h" for documentation
// of what the new port_<platform>.h file must provide.
#if defined(LEVELDB_PLATFORM_POSIX) || defined(LEVELDB_PLATFORM_WINDOWS)
#include "port/port_stdcxx.h"
#elif defined(LEVELDB_PLATFORM_CHROMIUM)
#include "port/port_chromium.h"
#endif

#endif  // STORAGE_LEVELDB_PORT_PORT_H_
