/*
 * This file is part of the EasyLogger Library.
 *
 * Copyright (c) 2015, Armink, <armink.ztl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Function: Portable interface for RT-Thread.
 * Created on: 2015-04-28
 */

#include <elog.h>
//#include <elog_flash.h>
#include "board.h"

static sem_t output_lock;

#define _ASYNC_OUTPUT_ENABLE
#define _ASYNC_OUTPUT_BUF_CNT       (8)
#define _ASYNC_OUTPUT_STACK_SIZE    (0x400)

#ifdef ELOG_ASYNC_OUTPUT_ENABLE
static sem_t output_notice;

static void async_output(void *arg);
#endif

#ifdef _ASYNC_OUTPUT_ENABLE
typedef struct {
    char log[ELOG_LINE_BUF_SIZE+1];
    int size;
} elog_t;
static rt_mutex_t s_elog_async_mutex = RT_NULL;
static rt_sem_t s_elog_async_sem = RT_NULL;
static elog_t s_async_log = {"", 0};

static void async_output(void *arg);
#endif

/**
 * EasyLogger port initialize
 *
 * @return result
 */
ElogErrCode elog_port_init(void) {
    ElogErrCode result = ELOG_NO_ERR;

    rt_sem_init(&output_lock, "elog lock", 1, RT_IPC_FLAG_PRIO);
    
#ifdef ELOG_ASYNC_OUTPUT_ENABLE
    static struct rt_thread async_thread;
    
    rt_sem_init(&output_notice, "elog async", 0, RT_IPC_FLAG_PRIO);

    if (RT_EOK == rt_thread_init(
            &async_thread, "elog_async", 
            async_output, NULL, 
            NULL, 2048,
            RT_THREAD_PRIORITY_MAX-3, 10)) 
    {
        rt_thread_startup(&async_thread);
    }
#endif
#ifdef _ASYNC_OUTPUT_ENABLE
    static struct rt_thread async_thread;
    
    if (RT_EOK == rt_thread_init(
            &async_thread, "elog_async", 
            async_output, NULL, 
            NULL, 2048,
            0, 10)) 
    {
        rt_thread_startup(&async_thread);
    }
#endif


    return result;
}

/**
 * output log port interface
 *
 * @param log output of log
 * @param size log size
 */
#include "time.h"
rt_bool_t storage_log_add(int elvl, const char *log, int size);
static rt_bool_t _s_in_output = RT_FALSE;

static void _port_output(const char *log, size_t size, int postflag) {
    size_t tag_len = 0;
    char tag_buf[ELOG_FILTER_TAG_MAX_LEN+1];
    /* output to terminal */
    rt_kprintf("%.*s\n", (int)size, log);

    if(!_s_in_output && rt_thread_self()) {
        _s_in_output = RT_TRUE;
        /* output to flash and server */
        int8_t lvl = elog_find_lvl(log);
        if(postflag) {
            /*const char *tag = elog_find_tag(log, lvl, &tag_len);
            if(tag_len > 0 && tag) {
                memset(tag_buf, 0, sizeof(tag_buf));
                memcpy(tag_buf, tag, tag_len);
                log_upload(lvl, tag_buf, log, size, time(0));
            }*/
        }
        if(lvl <= ELOG_LVL_INFO) {
            storage_log_add(lvl, log, size);
        }
    }
    _s_in_output = RT_FALSE;
}

void elog_port_output(const char *log, size_t size) {
#ifdef _ASYNC_OUTPUT_ENABLE
    if(s_elog_async_mutex && s_elog_async_sem) {
        rt_mutex_take(s_elog_async_mutex, RT_WAITING_FOREVER);
        {
            if(size > ELOG_LINE_BUF_SIZE) size = ELOG_LINE_BUF_SIZE;
            rt_strncpy(s_async_log.log, log, size);
            s_async_log.log[size] = '\0';
            s_async_log.size = size;
        }
        rt_sem_release(s_elog_async_sem);
        rt_thread_delay(rt_tick_from_millisecond(100));
        rt_mutex_release(s_elog_async_mutex);
    } else {
        _port_output(log, size, 0);
    }
#else
    _port_output(log, size, 1);
#endif
}

/**
 * output lock
 */
void elog_port_output_lock(void) {
    rt_sem_take(&output_lock, RT_WAITING_FOREVER);
}

/**
 * output unlock
 */
void elog_port_output_unlock(void) {
    rt_sem_release(&output_lock);
}

/**
 * get current time interface
 *
 * @return current time
 */
const char *elog_port_get_time(void) {
    static char cur_system_time[16] = { 0 };
    uint64_t t_now = das_sys_msectime();
    rt_snprintf(cur_system_time, 16, "tick:%08d.%03d", (int)(t_now / 1000), (int)(t_now % 1000));
    return cur_system_time;
}

/**
 * get current process name interface
 *
 * @return current process name
 */
const char *elog_port_get_p_info(void) {
    return "";
}

/**
 * get current thread name interface
 *
 * @return current thread name
 */
const char *elog_port_get_t_info(void) {
    rt_thread_t th = rt_thread_self();
    return th?th->name:"";
}

#ifdef ELOG_ASYNC_OUTPUT_ENABLE
void elog_async_output_notice(void) {
    rt_sem_release(&output_notice);
}

static void async_output(void *arg) {
    size_t get_log_size = 0;
    static char poll_get_buf[ELOG_LINE_BUF_SIZE - 4];

    while(true) {
        /* waiting log */
        rt_sem_take(&output_notice, RT_WAITING_FOREVER);
        /* polling gets and outputs the log */
        while(true) {

#ifdef ELOG_ASYNC_LINE_OUTPUT
            get_log_size = elog_async_get_line_log(poll_get_buf, sizeof(poll_get_buf));
#else
            get_log_size = elog_async_get_log(poll_get_buf, sizeof(poll_get_buf));
#endif

            if (get_log_size) {
                elog_port_output(poll_get_buf, get_log_size);
            } else {
                break;
            }
        }
    }
}
#endif

#ifdef _ASYNC_OUTPUT_ENABLE

static void async_output(void *arg) {
    s_elog_async_mutex = rt_mutex_create("elog_async", RT_IPC_FLAG_FIFO);
    s_elog_async_sem = rt_sem_create("elog_async", 0, RT_IPC_FLAG_FIFO);
    while(s_elog_async_mutex && s_elog_async_sem) {
        rt_mutex_take(s_elog_async_mutex, RT_WAITING_FOREVER);
        rt_mutex_release(s_elog_async_mutex);
        rt_sem_take(s_elog_async_sem, RT_WAITING_FOREVER);
        rt_mutex_take(s_elog_async_mutex, RT_WAITING_FOREVER);
        if(s_async_log.size > 0) {
            _port_output(s_async_log.log, s_async_log.size, 1);
        }
        rt_mutex_release(s_elog_async_mutex);
    }
}
#endif

