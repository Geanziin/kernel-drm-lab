/* SPDX-License-Identifier: MIT */
#ifndef __NVKM_R615_LIFECYCLE_H__
#define __NVKM_R615_LIFECYCLE_H__

struct nvkm_gsp;
enum nvkm_suspend_state;

int r615_gsp_oneinit(struct nvkm_gsp *gsp);
int r615_gsp_init(struct nvkm_gsp *gsp);
int r615_gsp_fini(struct nvkm_gsp *gsp, enum nvkm_suspend_state suspend);
void r615_gsp_dtor(struct nvkm_gsp *gsp);
void r615_gsp_save_logs(struct nvkm_gsp *gsp);
struct nvkm_gsp_fwif;
int r615_gsp_load(struct nvkm_gsp *gsp, int ver, const struct nvkm_gsp_fwif *fwif);

#endif
