/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "host/ble_monitor.h"

#if BLE_MONITOR

#if MYNEWT_VAL(BLE_MONITOR_UART) && MYNEWT_VAL(BLE_MONITOR_RTT)
#error "Cannot enable monitor over UART and RTT at the same time!"
#endif

#include <stdio.h>
#include <inttypes.h>
#include "os/os.h"
#include "log/log.h"
#if MYNEWT_VAL(BLE_MONITOR_UART)
#include "uart/uart.h"
#endif
#if MYNEWT_VAL(BLE_MONITOR_RTT)
#include "rtt/SEGGER_RTT.h"
#endif
#include "ble_monitor_priv.h"

/* UTC Timestamp for Jan 2016 00:00:00 */
#define UTC01_01_2016    1451606400

struct os_mutex lock;

#if MYNEWT_VAL(BLE_MONITOR_UART)
struct uart_dev *uart;

static uint8_t tx_ringbuf[64];
static uint8_t tx_ringbuf_head;
static uint8_t tx_ringbuf_tail;
#endif

#if MYNEWT_VAL(BLE_MONITOR_RTT)
static uint8_t rtt_buf[256];
static int rtt_index;
#if MYNEWT_VAL(BLE_MONITOR_RTT_BUFFERED)
static uint8_t rtt_pktbuf[256];
static size_t rtt_pktbuf_pos;
#endif
#endif

#if MYNEWT_VAL(BLE_MONITOR_UART)
static inline int
inc_and_wrap(int i, int max)
{
    return (i + 1) & (max - 1);
}

static int
monitor_uart_tx_char(void *arg)
{
    uint8_t ch;

    /* No more data */
    if (tx_ringbuf_head == tx_ringbuf_tail) {
        return -1;
    }

    ch = tx_ringbuf[tx_ringbuf_tail];
    tx_ringbuf_tail = inc_and_wrap(tx_ringbuf_tail, sizeof(tx_ringbuf));

    return ch;
}

static void
monitor_uart_queue_char(uint8_t ch)
{
    int sr;

    OS_ENTER_CRITICAL(sr);

    /* We need to try flush some data from ringbuffer if full */
    while (inc_and_wrap(tx_ringbuf_head, sizeof(tx_ringbuf)) ==
            tx_ringbuf_tail) {
        uart_start_tx(uart);
        OS_EXIT_CRITICAL(sr);
        OS_ENTER_CRITICAL(sr);
    }

    tx_ringbuf[tx_ringbuf_head] = ch;
    tx_ringbuf_head = inc_and_wrap(tx_ringbuf_head, sizeof(tx_ringbuf));

    OS_EXIT_CRITICAL(sr);
}

static void
monitor_write(const void *buf, size_t len)
{
    const uint8_t *ch = buf;

    while (len--) {
        monitor_uart_queue_char(*ch++);
    }

    uart_start_tx(uart);
}
#endif

#if MYNEWT_VAL(BLE_MONITOR_RTT)
static void
monitor_write(const void *buf, size_t len)
{
#if MYNEWT_VAL(BLE_MONITOR_RTT_BUFFERED)
    struct ble_monitor_hdr *hdr = (struct ble_monitor_hdr *) rtt_pktbuf;
    bool discard;

    /* We will discard any packet which exceeds length of intermediate buffer */
    discard = rtt_pktbuf_pos + len > sizeof(rtt_pktbuf);

    if (!discard) {
        memcpy(rtt_pktbuf + rtt_pktbuf_pos, buf, len);
    }

    rtt_pktbuf_pos += len;
    if (rtt_pktbuf_pos < sizeof(hdr->data_len) + hdr->data_len) {
        return;
    }

    // TODO: count dropped packets
    if (!discard) {
        SEGGER_RTT_WriteNoLock(rtt_index, rtt_pktbuf, rtt_pktbuf_pos);
    }

    rtt_pktbuf_pos = 0;
#else
    SEGGER_RTT_WriteNoLock(rtt_index, buf, len);
#endif
}
#endif

static size_t btmon_write(FILE *instance, const char *bp, size_t n)
{
    monitor_write(bp, n);

    return n;
}

static FILE *btmon = (FILE *) &(struct File) {
    .vmt = &(struct File_methods) {
        .write = btmon_write,
    },
};

static void
encode_monitor_hdr(struct ble_monitor_hdr *hdr, int64_t ts, uint16_t opcode,
                   uint16_t len)
{
    int rc;
    struct os_timeval tv;

    hdr->hdr_len  = sizeof(hdr->type) + sizeof(hdr->ts32);
    hdr->data_len = htole16(4 + hdr->hdr_len + len);
    hdr->opcode   = htole16(opcode);
    hdr->flags    = 0;

    /* Calculate timestamp if not present (same way as used in log module) */
    if (ts < 0) {
        rc = os_gettimeofday(&tv, NULL);
        if (rc || tv.tv_sec < UTC01_01_2016) {
            ts = os_get_uptime_usec();
        } else {
            ts = tv.tv_sec * 1000000 + tv.tv_usec;
        }
    }

    /* Extended header */
    hdr->type = BLE_MONITOR_EXTHDR_TS32;
    hdr->ts32 = htole32(ts / 100);
}

int
ble_monitor_init(void)
{
#if MYNEWT_VAL(BLE_MONITOR_UART)
    struct uart_conf uc = {
        .uc_speed = MYNEWT_VAL(BLE_MONITOR_UART_BAUDRATE),
        .uc_databits = 8,
        .uc_stopbits = 1,
        .uc_parity = UART_PARITY_NONE,
        .uc_flow_ctl = UART_FLOW_CTL_NONE,
        .uc_tx_char = monitor_uart_tx_char,
        .uc_rx_char = NULL,
        .uc_cb_arg = NULL,
    };

    uart = (struct uart_dev *)os_dev_open("uart0", OS_TIMEOUT_NEVER,
                                             &uc);
    if (!uart) {
        return -1;
    }
#endif

#if MYNEWT_VAL(BLE_MONITOR_RTT)
#if MYNEWT_VAL(BLE_MONITOR_RTT_BUFFERED)
    rtt_index = SEGGER_RTT_AllocUpBuffer("monitor", rtt_buf, sizeof(rtt_buf),
                                         SEGGER_RTT_MODE_NO_BLOCK_SKIP);
#else
    rtt_index = SEGGER_RTT_AllocUpBuffer("monitor", rtt_buf, sizeof(rtt_buf),
                                         SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL);
#endif

    if (rtt_index < 0) {
        return -1;
    }
#endif

    os_mutex_init(&lock);

    return 0;
}

int
ble_monitor_send(uint16_t opcode, const void *data, size_t len)
{
    struct ble_monitor_hdr hdr;

    encode_monitor_hdr(&hdr, -1, opcode, len);

    os_mutex_pend(&lock, OS_TIMEOUT_NEVER);

    monitor_write(&hdr, sizeof(hdr));
    monitor_write(data, len);

    os_mutex_release(&lock);

    return 0;
}

int
ble_monitor_send_om(uint16_t opcode, const struct os_mbuf *om)
{
    const struct os_mbuf *om_tmp;
    struct ble_monitor_hdr hdr;
    uint16_t length = 0;

    om_tmp = om;
    while (om_tmp) {
        length += om_tmp->om_len;
        om_tmp = SLIST_NEXT(om_tmp, om_next);
    }

    encode_monitor_hdr(&hdr, -1, opcode, length);

    os_mutex_pend(&lock, OS_TIMEOUT_NEVER);

    monitor_write(&hdr, sizeof(hdr));

    while (om) {
        monitor_write(om->om_data, om->om_len);
        om = SLIST_NEXT(om, om_next);
    }

    os_mutex_release(&lock);

    return 0;
}

int
ble_monitor_new_index(uint8_t bus, uint8_t *addr, const char *name)
{
    struct ble_monitor_new_index pkt;

    pkt.type = 0; /* Primary controller, we don't support other */
    pkt.bus = bus;
    memcpy(pkt.bdaddr, addr, 6);
    strncpy(pkt.name, name, sizeof(pkt.name) - 1);
    pkt.name[sizeof(pkt.name) - 1] = '\0';

    ble_monitor_send(BLE_MONITOR_OPCODE_NEW_INDEX, &pkt, sizeof(pkt));

    return 0;
}

int
ble_monitor_log(int level, const char *fmt, ...)
{
    struct ble_monitor_hdr hdr;
    struct ble_monitor_user_logging ulog;
    const char id[] = "nimble";
    va_list va;
    int len;

    va_start(va, fmt);
    len = vsprintf(NULL, fmt, va);
    va_end(va);

    switch (level) {
    case LOG_LEVEL_ERROR:
        ulog.priority = 3;
        break;
    case LOG_LEVEL_WARN:
        ulog.priority = 4;
        break;
    case LOG_LEVEL_INFO:
        ulog.priority = 6;
        break;
    case LOG_LEVEL_DEBUG:
        ulog.priority = 7;
        break;
    default:
        ulog.priority = 8;
        break;
    }

    ulog.ident_len = sizeof(id);

    encode_monitor_hdr(&hdr, -1, BLE_MONITOR_OPCODE_USER_LOGGING,
                       sizeof(ulog) + sizeof(id) + len + 1);

    os_mutex_pend(&lock, OS_TIMEOUT_NEVER);

    monitor_write(&hdr, sizeof(hdr));
    monitor_write(&ulog, sizeof(ulog));
    monitor_write(id, sizeof(id));

    va_start(va, fmt);
    vfprintf(btmon, fmt, va);
    va_end(va);

    /* null-terminate string */
    monitor_write("", 1);

    os_mutex_release(&lock);

    return 0;
}

int
ble_monitor_out(int c)
{
    static char buf[128];
    static size_t len;

    if (c != '\n' && len < sizeof(buf) - 1) {
        buf[len++] = c;
        return c;
    }

    buf[len++] = '\0';

    ble_monitor_send(BLE_MONITOR_OPCODE_SYSTEM_NOTE, buf, len);
    len = 0;

    return c;
}

#endif