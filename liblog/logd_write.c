/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <time.h>
#include <stdio.h>
#ifdef HAVE_PTHREADS
#include <pthread.h>
#endif
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef MOTOROLA_LOG
#if HAVE_LIBC_SYSTEM_PROPERTIES
#include <sys/system_properties.h>
#endif
#endif

#include <log/logger.h>
#include <log/logd.h>
#include <cutils/log.h>

#define LOG_BUF_SIZE	1024

#if FAKE_LOG_DEVICE
// This will be defined when building for the host.
#define log_open(pathname, flags) fakeLogOpen(pathname, flags)
#define log_writev(filedes, vector, count) fakeLogWritev(filedes, vector, count)
#define log_close(filedes) fakeLogClose(filedes)
#else
#define log_open(pathname, flags) open(pathname, (flags) | O_CLOEXEC)
#define log_writev(filedes, vector, count) writev(filedes, vector, count)
#define log_close(filedes) close(filedes)
#endif

static int __write_to_log_init(log_id_t, struct iovec *vec, size_t nr);
static int (*write_to_log)(log_id_t, struct iovec *vec, size_t nr) = __write_to_log_init;
#ifdef HAVE_PTHREADS
static pthread_mutex_t log_init_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static int log_fds[(int)LOG_ID_MAX] = { -1, -1, -1, -1 };

/*
 * This is used by the C++ code to decide if it should write logs through
 * the C code.  Basically, if /dev/log/... is available, we're running in
 * the simulator rather than a desktop tool and want to use the device.
 */
static enum {
    kLogUninitialized, kLogNotAvailable, kLogAvailable
} g_log_status = kLogUninitialized;
int __android_log_dev_available(void)
{
    if (g_log_status == kLogUninitialized) {
        if (access("/dev/"LOGGER_LOG_MAIN, W_OK) == 0)
            g_log_status = kLogAvailable;
        else
            g_log_status = kLogNotAvailable;
    }

    return (g_log_status == kLogAvailable);
}

#ifdef HTCLOG
signed int __htclog_read_masks(char *buf, signed int len)
{
    return 0;
}

int __htclog_init_mask(const char *a1, unsigned int a2, int a3)
{
    return 0;
}

int __htclog_print_private(int a1, const char *a2, const char *fmt, ...)
{
    return 0;
}
#endif

#ifdef MOTOROLA_LOG
/* Fallback when there is neither log.tag.<tag> nor log.tag.DEFAULT.
 * this is compile-time defaulted to "info". The log startup code
 * looks at the build tags to see about whether it should be DEBUG...
 * -- just as is done in frameworks/base/core/jni/android_util_Log.cpp
 */
static int prio_fallback = ANDROID_LOG_INFO;

/*
 * public interface so native code can see "should i log this"
 * and behave similar to java Log.isLoggable() calls.
 *
 * NB: we have (level,tag) here to match the other __android_log entries.
 * The Java side uses (tag,level) for its ordering.
 * since the args are (int,char*) vs (char*,char*) we won't get strange
 * swapped-the-strings errors.
 */

#define	LOGGING_PREFIX	"log.tag."
#define	LOGGING_DEFAULT	"log.tag.DEFAULT"

int __android_log_loggable(int prio, const char *tag)
{
    int nprio;

#if	HAVE_LIBC_SYSTEM_PROPERTIES
    char keybuf[PROP_NAME_MAX];
    char results[PROP_VALUE_MAX];
    int n;

    /* we can NOT cache the log.tag.<tag> and log.tag.DEFAULT
     * values because either one can be changed dynamically.
     *
     * damn, says the performance compulsive.
     */

    n = 0;
    results[0] = '\0';
    if (tag) {
	memcpy (keybuf, LOGGING_PREFIX, strlen (LOGGING_PREFIX) + 1);
	/* watch out for buffer overflow */
	strncpy (keybuf + strlen (LOGGING_PREFIX), tag,
		 sizeof (keybuf) - strlen (LOGGING_PREFIX));
	keybuf[sizeof (keybuf) - 1] = '\0';
	n = __system_property_get (keybuf, results);
    }
    if (n == 0) {
	/* nothing yet, look for the global */
	memcpy (keybuf, LOGGING_DEFAULT, sizeof (LOGGING_DEFAULT));
	n = __system_property_get (keybuf, results);
    }

    if (n == 0) {
	nprio = prio_fallback;
    } else {
	switch (results[0])
	{
	case 'E':
	    nprio = ANDROID_LOG_ERROR;
	    break;
	case 'W':
	    nprio = ANDROID_LOG_WARN;
	    break;
	case 'I':
	    nprio = ANDROID_LOG_INFO;
	    break;
	case 'D':
	    nprio = ANDROID_LOG_DEBUG;
	    break;
	case 'V':
	    nprio = ANDROID_LOG_VERBOSE;
	    break;
	case 'S':
	    nprio = ANDROID_LOG_SILENT;
	    break;
	default:
	    /* unspecified or invalid */
	    nprio = prio_fallback;
	    break;
	}
    }
#else
    /* no system property routines, fallback to a default */
    nprio = prio_fallback;
#endif

    return ((prio >= nprio) ? 1 : 0);
}
#endif

static int __write_to_log_null(log_id_t log_fd, struct iovec *vec, size_t nr)
{
    return -1;
}

static int __write_to_log_kernel(log_id_t log_id, struct iovec *vec, size_t nr)
{
    ssize_t ret;
    int log_fd;

    if (/*(int)log_id >= 0 &&*/ (int)log_id < (int)LOG_ID_MAX) {
        log_fd = log_fds[(int)log_id];
    } else {
        return EBADF;
    }

    do {
        ret = log_writev(log_fd, vec, nr);
    } while (ret < 0 && errno == EINTR);

    return ret;
}

static int __write_to_log_init(log_id_t log_id, struct iovec *vec, size_t nr)
{
#ifdef HAVE_PTHREADS
    pthread_mutex_lock(&log_init_lock);
#endif

    if (write_to_log == __write_to_log_init) {
        log_fds[LOG_ID_MAIN] = log_open("/dev/"LOGGER_LOG_MAIN, O_WRONLY);
        log_fds[LOG_ID_RADIO] = log_open("/dev/"LOGGER_LOG_RADIO, O_WRONLY);
        log_fds[LOG_ID_EVENTS] = log_open("/dev/"LOGGER_LOG_EVENTS, O_WRONLY);
        log_fds[LOG_ID_SYSTEM] = log_open("/dev/"LOGGER_LOG_SYSTEM, O_WRONLY);

        write_to_log = __write_to_log_kernel;

        if (log_fds[LOG_ID_MAIN] < 0 || log_fds[LOG_ID_RADIO] < 0 ||
                log_fds[LOG_ID_EVENTS] < 0) {
            log_close(log_fds[LOG_ID_MAIN]);
            log_close(log_fds[LOG_ID_RADIO]);
            log_close(log_fds[LOG_ID_EVENTS]);
            log_fds[LOG_ID_MAIN] = -1;
            log_fds[LOG_ID_RADIO] = -1;
            log_fds[LOG_ID_EVENTS] = -1;
            write_to_log = __write_to_log_null;
        }

        if (log_fds[LOG_ID_SYSTEM] < 0) {
            log_fds[LOG_ID_SYSTEM] = log_fds[LOG_ID_MAIN];
        }
    }

#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&log_init_lock);
#endif

    return write_to_log(log_id, vec, nr);
}

int __android_log_write(int prio, const char *tag, const char *msg)
{
    struct iovec vec[3];
    log_id_t log_id = LOG_ID_MAIN;
    char tmp_tag[32];

    if (!tag)
        tag = "";

    /* XXX: This needs to go! */
    if (!strcmp(tag, "HTC_RIL") ||
        !strncmp(tag, "RIL", 3) || /* Any log tag with "RIL" as the prefix */
        !strncmp(tag, "IMS", 3) || /* Any log tag with "IMS" as the prefix */
        !strcmp(tag, "AT") ||
        !strcmp(tag, "GSM") ||
        !strcmp(tag, "STK") ||
        !strcmp(tag, "CDMA") ||
        !strcmp(tag, "PHONE") ||
        !strcmp(tag, "SMS") ||
        !strcmp(tag, "KINETO") ||
        !strncmp(tag, "KIPC", 4) ||
        !strncmp(tag, "Kineto", 6) ||
        !strncmp(tag, "QCRIL", 5) ||
        !strncmp(tag, "QC-RIL", 6) ||
        !strncmp(tag, "QC-QMI", 6) ||
        !strncmp(tag, "QC-ONCRPC", 9) ||
        !strncmp(tag, "QC-DSI", 6) ||
        !strcmp(tag, "QC-NETMGR-LIB") ||
        !strcmp(tag, "QC-QDP") ||
        !strcmp(tag, "Diag_Lib")
        ) {
            log_id = LOG_ID_RADIO;
            // Inform third party apps/ril/radio.. to use Rlog or RLOG
            snprintf(tmp_tag, sizeof(tmp_tag), "use-Rlog/RLOG-%s", tag);
            tag = tmp_tag;
    }

    vec[0].iov_base   = (unsigned char *) &prio;
    vec[0].iov_len    = 1;
    vec[1].iov_base   = (void *) tag;
    vec[1].iov_len    = strlen(tag) + 1;
    vec[2].iov_base   = (void *) msg;
    vec[2].iov_len    = strlen(msg) + 1;

    return write_to_log(log_id, vec, 3);
}

int __android_log_buf_write(int bufID, int prio, const char *tag, const char *msg)
{
    struct iovec vec[3];
    char tmp_tag[32];

    if (!tag)
        tag = "";

    /* XXX: This needs to go! */
    if ((bufID != LOG_ID_RADIO) &&
         (!strcmp(tag, "HTC_RIL") ||
        !strncmp(tag, "RIL", 3) || /* Any log tag with "RIL" as the prefix */
        !strncmp(tag, "IMS", 3) || /* Any log tag with "IMS" as the prefix */
        !strcmp(tag, "AT") ||
        !strcmp(tag, "GSM") ||
        !strcmp(tag, "STK") ||
        !strcmp(tag, "CDMA") ||
        !strcmp(tag, "PHONE") ||
        !strcmp(tag, "SMS") ||
        !strcmp(tag, "KINETO") ||
        !strncmp(tag, "KIPC", 4) ||
        !strncmp(tag, "Kineto", 6) ||
        !strncmp(tag, "QCRIL", 5) ||
        !strncmp(tag, "QC-RIL", 6) ||
        !strncmp(tag, "QC-QMI", 6) ||
        !strncmp(tag, "QC-ONCRPC", 9) ||
        !strncmp(tag, "QC-DSI", 6)
        )) {
            bufID = LOG_ID_RADIO;
            // Inform third party apps/ril/radio.. to use Rlog or RLOG
            snprintf(tmp_tag, sizeof(tmp_tag), "use-Rlog/RLOG-%s", tag);
            tag = tmp_tag;
    }

    vec[0].iov_base   = (unsigned char *) &prio;
    vec[0].iov_len    = 1;
    vec[1].iov_base   = (void *) tag;
    vec[1].iov_len    = strlen(tag) + 1;
    vec[2].iov_base   = (void *) msg;
    vec[2].iov_len    = strlen(msg) + 1;

    return write_to_log(bufID, vec, 3);
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[LOG_BUF_SIZE];

    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);

    return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE];

    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);

    return __android_log_write(prio, tag, buf);
}

int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE];

    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);

    return __android_log_buf_write(bufID, prio, tag, buf);
}

void __android_log_assert(const char *cond, const char *tag,
			  const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
        va_end(ap);
    } else {
        /* Msg not provided, log condition.  N.B. Do not use cond directly as
         * format string as it could contain spurious '%' syntax (e.g.
         * "%d" in "blocks%devs == 0").
         */
        if (cond)
            snprintf(buf, LOG_BUF_SIZE, "Assertion failed: %s", cond);
        else
            strcpy(buf, "Unspecified assertion failed");
    }

    __android_log_write(ANDROID_LOG_FATAL, tag, buf);

    __builtin_trap(); /* trap so we have a chance to debug the situation */
}

int __android_log_bwrite(int32_t tag, const void *payload, size_t len)
{
    struct iovec vec[2];

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = (void*)payload;
    vec[1].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 2);
}

/*
 * Like __android_log_bwrite, but takes the type as well.  Doesn't work
 * for the general case where we're generating lists of stuff, but very
 * handy if we just want to dump an integer into the log.
 */
int __android_log_btwrite(int32_t tag, char type, const void *payload,
    size_t len)
{
    struct iovec vec[3];

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = &type;
    vec[1].iov_len = sizeof(type);
    vec[2].iov_base = (void*)payload;
    vec[2].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 3);
}
