/*
 * Copyright (c) 2015,2018 The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "QTI PowerHAL"
#include <hardware/hardware.h>
#include <hardware/power.h>
#include <log/log.h>

#include "hint-data.h"
#include "metadata-defs.h"
#include "performance.h"
#include "power-common.h"
#include "utils.h"

#define MIN_VAL(X, Y) ((X > Y) ? (Y) : (X))

static int video_encode_hint_sent;
static void process_video_encode_hint(void* metadata);
static int display_fd;
#define SYS_DISPLAY_PWR "/sys/kernel/hbtp/display_pwr"

/**
 * Returns true if the target is SDM439/SDM429.
 */
static bool is_target_SDM439(void) {
    static int is_SDM439 = -1;
    int soc_id;

    if (is_SDM439 >= 0) return is_SDM439;

    soc_id = get_soc_id();
    is_SDM439 = soc_id == 353 || soc_id == 363 || soc_id == 354 || soc_id == 364;

    return is_SDM439;
}

int power_hint_override(power_hint_t hint, void* data) {
    switch (hint) {
        case POWER_HINT_VSYNC:
            break;
        case POWER_HINT_VIDEO_ENCODE: {
            process_video_encode_hint(data);
            return HINT_HANDLED;
        }
        default:
            break;
    }
    return HINT_NONE;
}

int set_interactive_override(int on) {
    char governor[80];
    int rc = 0;

    static const char* display_on = "1";
    static const char* display_off = "0";
    char err_buf[80];
    static int init_interactive_hint = 0;
    static int set_i_count = 0;

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_HANDLED;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
        if (is_interactive_governor(governor)) {
            int resource_values[] = {INT_OP_CLUSTER0_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                                     INT_OP_CLUSTER1_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                                     INT_OP_NOTIFY_ON_MIGRATE,   0x00};

            perform_hint_action(DISPLAY_STATE_HINT_ID, resource_values,
                                ARRAY_SIZE(resource_values));
        } /* Perf time rate set for CORE0,CORE4 8952 target*/

    } else {
        /* Display on. */
        if (is_interactive_governor(governor)) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
        }
    }

    set_i_count++;
    ALOGI("Got set_interactive hint on= %d, count= %d\n", on, set_i_count);

    if (init_interactive_hint == 0) {
        // First time the display is turned off
        display_fd = TEMP_FAILURE_RETRY(open(SYS_DISPLAY_PWR, O_RDWR));
        if (display_fd < 0) {
            strerror_r(errno, err_buf, sizeof(err_buf));
            ALOGE("Error opening %s: %s\n", SYS_DISPLAY_PWR, err_buf);
            return HINT_HANDLED;
        } else
            init_interactive_hint = 1;
    } else if (!on) {
        /* Display off. */
        rc = TEMP_FAILURE_RETRY(write(display_fd, display_off, strlen(display_off)));
        if (rc < 0) {
            strerror_r(errno, err_buf, sizeof(err_buf));
            ALOGE("Error writing %s to  %s: %s\n", display_off, SYS_DISPLAY_PWR, err_buf);
        }
    } else {
        /* Display on */
        rc = TEMP_FAILURE_RETRY(write(display_fd, display_on, strlen(display_on)));
        if (rc < 0) {
            strerror_r(errno, err_buf, sizeof(err_buf));
            ALOGE("Error writing %s to  %s: %s\n", display_on, SYS_DISPLAY_PWR, err_buf);
        }
    }

    return HINT_HANDLED;
}

/* Video Encode Hint */
static void process_video_encode_hint(void* metadata) {
    char governor[80] = {0};
    int resource_values[20] = {0};
    struct video_encode_metadata_t video_encode_metadata;

    ALOGI("Got process_video_encode_hint");

    if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return;
                }
            }
        }
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (metadata) {
        if (parse_video_encode_metadata((char*)metadata, &video_encode_metadata) == -1) {
            ALOGE("Error occurred while parsing metadata.");
            return;
        }
    } else {
        return;
    }

    if (video_encode_metadata.state == 1) {
        if (is_schedutil_governor(governor)) {
            if (is_target_SDM439()) {
                /* sample_ms = 10mS
                 * SLB for Core0 = -6
                 * SLB for Core1 = -6
                 * SLB for Core2 = -6
                 * SLB for Core3 = -6
                 * hispeed load = 95
                 * hispeed freq = 998Mhz */
                int res[] = {
                        0x41820000, 0xa,        0x40c68100, 0xfffffffa, 0x40c68110,
                        0xfffffffa, 0x40c68120, 0xfffffffa, 0x40c68130, 0xfffffffa,
                        0x41440100, 0x5f,       0x4143c100, 0x3e6,
                };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                if (!video_encode_hint_sent) {
                    perform_hint_action(video_encode_metadata.hint_id, resource_values,
                                        ARRAY_SIZE(res));
                    video_encode_hint_sent = 1;
                }
            } else {
                /* sample_ms = 10mS */
                int res[] = {
                        0x41820000,
                        0xa,
                };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                if (!video_encode_hint_sent) {
                    perform_hint_action(video_encode_metadata.hint_id, resource_values,
                                        ARRAY_SIZE(res));
                    video_encode_hint_sent = 1;
                }
            }
        } else if (is_interactive_governor(governor)) {
            /* Sched_load and migration_notif*/
            int res[] = {INT_OP_CLUSTER0_USE_SCHED_LOAD,      0x1,
                         INT_OP_CLUSTER1_USE_SCHED_LOAD,      0x1,
                         INT_OP_CLUSTER0_USE_MIGRATION_NOTIF, 0x1,
                         INT_OP_CLUSTER1_USE_MIGRATION_NOTIF, 0x1,
                         INT_OP_CLUSTER0_TIMER_RATE,          BIG_LITTLE_TR_MS_40,
                         INT_OP_CLUSTER1_TIMER_RATE,          BIG_LITTLE_TR_MS_40};
            memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
            perform_hint_action(video_encode_metadata.hint_id, resource_values, ARRAY_SIZE(res));
            video_encode_hint_sent = 1;
        }
    } else if (video_encode_metadata.state == 0) {
        if (is_interactive_governor(governor) || is_schedutil_governor(governor)) {
            undo_hint_action(video_encode_metadata.hint_id);
            video_encode_hint_sent = 0;
        }
    }
    return;
}
