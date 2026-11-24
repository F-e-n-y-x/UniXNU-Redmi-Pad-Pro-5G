// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan's XNU (iPadOS 17.7, xnu-10063) has no per-task GPU role (task_get_gpu_role, PRIO_DARWIN_GPU_BACKGROUND/UTILITY), only
// RunningBoard's deny flag. Force-included into NativeAdrenoUserClient.cpp by scripts/build-ruan-adreno.py: a denied task counts as
// background, every other one as foreground (the driver's only use of the role).
#include <sys/resource.h>
#include <kern/task.h>
#define PRIO_DARWIN_GPU_BACKGROUND 0x103   // values the 17.7 kernel never returns
#define PRIO_DARWIN_GPU_UTILITY 0x104
#define task_get_gpu_role(task) (task_is_gpu_denied(task) ? PRIO_DARWIN_GPU_DENY : PRIO_DARWIN_GPU_ALLOW)
