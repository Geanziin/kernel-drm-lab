/* SPDX-License-Identifier: MIT */
#ifndef __NOUVEAU_GSP_TELEMETRY_H__
#define __NOUVEAU_GSP_TELEMETRY_H__
struct drm_device;
int nouveau_gsp_telemetry_init(struct drm_device *);
void nouveau_gsp_telemetry_fini(struct drm_device *);
#endif
