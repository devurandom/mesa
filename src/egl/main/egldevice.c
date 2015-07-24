/**************************************************************************
 *
 * Copyright 2015 Collabora
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


#include "egldevice.h"
#include "eglglobals.h"
#include "egltypedefs.h"


typedef struct {
   /* device list */
   _EGLDevice *devices;

} _EGLDeviceInfo;

static _EGLDeviceInfo *
_eglEnsureDeviceInfo(void)
{
   _EGLDeviceInfo *info;

   mtx_lock(_eglGlobal.Mutex);

   info = _eglGlobal.DeviceInfo;

   if (!info) {
      info = calloc(1, sizeof(_EGLDeviceInfo));
      if (!info)
         goto out;

      info->devices = NULL;

      _eglGlobal.DeviceInfo = info;
   }

out:
   mtx_unlock(_eglGlobal.Mutex);

   return info;
}

/**
 * Finish device management.
 */
void
_eglFiniDeviceInfo(void)
{
   _EGLDeviceInfo *info;
   _EGLDevice *device_list, *device;

   /* atexit function is called with global mutex locked */

   info = _eglGlobal.DeviceInfo;

   if (!info)
      return;

   device_list = info->devices;
   while (device_list) {
      /* pop list head */
      device = device_list;
      device_list = device_list->Next;

      free(device);
   }

   free(info);
   _eglGlobal.DeviceInfo = NULL;
}
