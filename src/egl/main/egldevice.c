/**************************************************************************
 *
 * Copyright 2015 Collabora
 * Copyright 2016 Red Hat, Inc.
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

#ifdef HAVE_LIBDRM
#include <xf86drm.h>
#endif

#include "eglcurrent.h"
#include "egldevice.h"
#include "eglglobals.h"
#include "egllog.h"
#include "egltypedefs.h"
#include "util/macros.h"

typedef struct {
   /* device list */
   _EGLDevice *devices;
   EGLBoolean got_devices;

#ifdef HAVE_LIBDRM
   drmDevicePtr *drm_devices;
   int num_drm_devices;
#endif

} _EGLDeviceInfo;

static EGLBoolean _eglFillDeviceList(_EGLDeviceInfo *info);

static _EGLDeviceInfo *
_eglEnsureDeviceInfo(EGLBoolean get_devices)
{
   _EGLDeviceInfo *info;

   mtx_lock(_eglGlobal.Mutex);

   info = _eglGlobal.DeviceInfo;

   if (!info) {
      info = calloc(1, sizeof(_EGLDeviceInfo));
      if (!info)
         goto out;

      info->devices = NULL;
      info->got_devices = EGL_FALSE;

      _eglGlobal.DeviceInfo = info;
   }

   if (get_devices && !info->got_devices) {
      if (!_eglFillDeviceList(info))
         info = NULL;
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

#ifdef HAVE_LIBDRM
   drmFreeDevices(info->drm_devices, info->num_drm_devices);
   free(info->drm_devices);
#endif

   free(info);
   _eglGlobal.DeviceInfo = NULL;
}

static EGLBoolean
_eglFillDeviceList(_EGLDeviceInfo *info)
{
#ifdef HAVE_LIBDRM
   info->num_drm_devices = drmGetDevices(NULL, 0);

   if (info->num_drm_devices < 0) {
      info->num_drm_devices = 0;
      return EGL_FALSE;
   }

   info->drm_devices = calloc(info->num_drm_devices, sizeof(drmDevicePtr));
   if (!info->drm_devices) {
      info->num_drm_devices = 0;
      return EGL_FALSE;
   }

   if (drmGetDevices(info->drm_devices, info->num_drm_devices) < 0) {
      free(info->drm_devices);
      info->num_drm_devices = 0;
      return EGL_FALSE;
   }

   info->got_devices = EGL_TRUE;
   return EGL_TRUE;
#else
   return EGL_FALSE;
#endif
}

/**
 * Enumerate EGL devices.
 */
EGLBoolean
_eglQueryDevicesEXT(EGLint max_devices,
                    _EGLDevice **devices,
                    EGLint *num_devices)
{
   _EGLDeviceInfo *info;
   EGLBoolean ret = EGL_TRUE;
   int i;
   _EGLDevice *dev;

   /* max_devices can only be bad if devices is non-NULL. num_devices must
    * always be present. */
   if ((devices && max_devices < 1) || !num_devices)
      return _eglError(EGL_BAD_PARAMETER, "eglQueryDevicesEXT");

   info = _eglEnsureDeviceInfo(EGL_TRUE);
   if (!info)
      return _eglError(EGL_BAD_ALLOC, "eglQueryDevicesEXT");

   mtx_lock(_eglGlobal.Mutex);

   /* work out total number of devices */
   for (i = 0, dev = info->devices; dev; i++, dev = dev->Next)
      ;

   /* bail early if we devices is NULL */
   if (!devices) {
      *num_devices = i;
      goto out;
   }

   /* create and fill devices array */
   *num_devices = MIN2(i, max_devices);

   for (i = 0, dev = info->devices;
        i < *num_devices;
        i++, dev = dev->Next) {
      devices[i] = dev;
   }

out:
   mtx_unlock(_eglGlobal.Mutex);

   return ret;
}
