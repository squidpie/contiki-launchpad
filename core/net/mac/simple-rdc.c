/*
 * Copyright (c) 2012
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/**
 * \file
 *         A simple radio duty cycling layer, meant to be a low-complexity and
 *         low-RAM alternative to ContikiMAC.
 *         
 * \author
 *         Marcus Lunden <marcus.lunden@gmail.com>
 */

/*
 * Copyright (c) 2010, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

/**
 * \file
 *         Implementation of the ContikiMAC power-saving radio duty cycling protocol
 * \author
 *         Adam Dunkels <adam@sics.se>
 *         Niclas Finne <nfi@sics.se>
 *         Joakim Eriksson <joakime@sics.se>
 */


#include <stdio.h>
#include <string.h>
#include "contiki.h"
#include "contiki-conf.h"
#include "net/rime.h"
#include "net/netstack.h"
#include "net/mac/simple-rdc.h"
#include "dev/radio.h"
#include "dev/watchdog.h"
#include "dev/leds.h"
#include "sys/pt.h"
#include "sys/rtimer.h"
#include "sys/clock.h"


#define DEBUG 1
#if DEBUG
#include <stdio.h>
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

#if 0
| = on/tx
- = on/Rx
_ = off


Normal duty cycling, to traffic

    ____________+--+____________+--+____________+--+____________
                        tOFF     tON



Transmitting

    ____________+--+______|_|_|_|_|_|_|_|_|_|___+--+____________
                                           ^tBTx
                          |-------tTX-------|


tON     SIMPLERDC_ONTIME
tOFF    deduced from SIMPLERDC_ONTIME and SIMPLERDC_CHECKRATE
tTX     deduced from SIMPLERDC_CHECKRATE and SIMPLERDC_ONTIME, for sending 
          little longer than just the sleep-period
tBTx    AFTER_TX_HOLDOFF, the time it takes for a packet to be received and ACKed.
#endif /* if 0; commented out code */



/*---------------------------------------------------------------------------*/
/* SimpleRDC simple configuration */
#define SIMPLERDC_CHECKRATE       8                         // in Hz, power of two
#define SIMPLERDC_ONTIME          (CLOCK_SECOND / 128)

/* deduced configurations and definitions, not to be changed */
#define SIMPLERDC_OFFTIME         (CLOCK_SECOND / SIMPLERDC_CHECKRATE - SIMPLERDC_ONTIME)
#define BETWEEN_TX_TIME           (RTIMER_SECOND/1000)

// for how long to transmit (broadcast, *casts stop at ACK)
#define TX_PERIOD                 ((CLOCK_SECOND / SIMPLERDC_CHECKRATE) + (2 * SIMPLERDC_ONTIME))

// if in tx and waiting for an ACK, and we detect traffic, we wait this long to see if it is indeed an ACK
#define ACK_DETECT_WAIT_TIME      ((2 * RTIMER_SECOND)/1000)

#define TX_COUNT  10    // XXX to be deduced from timings instead
/*
minimum transmissions = ontime / (txtime + waittime)
  where txtime = txbase + txtime(bytes)
  and waittime < SIMPLERDC_ONTIME * 0.8 or sth
*/
/*---------------------------------------------------------------------------*/

#define BUSYWAIT_UNTIL(cond, max_time)                                  \
  do {                                                                  \
    rtimer_clock_t t0;                                                  \
    t0 = RTIMER_NOW();                                                  \
    while(!(cond) && RTIMER_CLOCK_LT(RTIMER_NOW(), t0 + (max_time)));   \
  } while(0)



/* Are we currently receiving a burst? */
static uint8_t we_are_receiving_burst = 0;
/* Has the receiver been awoken by a burst we're sending? */
static uint8_t is_receiver_awake = 0;

/*static struct rtimer rt;*/
/*static struct pt pt;*/

static volatile uint8_t simplerdc_is_on = 0;
static volatile uint8_t simplerdc_keep_radio_on = 0;

static volatile unsigned char we_are_sending = 0;
static volatile unsigned char radio_is_on = 0;


// XXX reduce or remove XXX XXX 4B per
struct seqno {
  rimeaddr_t sender;
  uint8_t seqno;
};
#ifdef NETSTACK_CONF_MAC_SEQNO_HISTORY
#define MAX_SEQNOS NETSTACK_CONF_MAC_SEQNO_HISTORY
#else /* NETSTACK_CONF_MAC_SEQNO_HISTORY */
#define MAX_SEQNOS 16
#endif /* NETSTACK_CONF_MAC_SEQNO_HISTORY */
#undef MAX_SEQNOS
#define MAX_SEQNOS 2
static struct seqno received_seqnos[MAX_SEQNOS];

/* Radio does CSMA and autobackoff */
#ifdef RDC_CONF_HARDWARE_CSMA
#if RDC_CONF_HARDWARE_CSMA
#warning "SimpleRDC assumes no hardware CSMA present, does explicit check. Just FYI."
#endif
#endif

/* Radio returns TX_OK/TX_NOACK after autoack wait */
#ifndef RDC_CONF_HARDWARE_ACK
#define RDC_CONF_HARDWARE_ACK        0
#endif

/* CYCLE_TIME for channel cca checks, in rtimer ticks. */
#ifdef SIMPLERDC_CONF_CYCLE_TIME
#define CYCLE_TIME (SIMPLERDC_CONF_CYCLE_TIME)
#else
#define CYCLE_TIME (RTIMER_ARCH_SECOND / NETSTACK_RDC_CHANNEL_CHECK_RATE)
#endif

/* CHANNEL_CHECK_RATE is enforced to be a power of two.
 * If RTIMER_ARCH_SECOND is not also a power of two, there will be an inexact
 * number of channel checks per second due to the truncation of CYCLE_TIME.
 * This will degrade the effectiveness of phase optimization with neighbors that
 * do not have the same truncation error.
 * Define SYNC_CYCLE_STARTS to ensure an integral number of checks per second.
 */
#if RTIMER_ARCH_SECOND & (RTIMER_ARCH_SECOND - 1)
#define SYNC_CYCLE_STARTS                    1
#endif

/* BURST_RECV_TIME is the maximum time a receiver waits for the
   next packet of a burst when FRAME_PENDING is set. */
#define INTER_PACKET_DEADLINE               CLOCK_SECOND / 32

/* simplerdc performs periodic channel checks. Each channel check
   consists of two or more CCA checks. CCA_COUNT_MAX is the number of
   CCAs to be done for each periodic channel check. The default is
   two.*/
#define CCA_COUNT_MAX                      2

/* Before starting a transmission, simplerdc checks the availability
   of the channel with CCA_COUNT_MAX_TX consecutive CCAs */
#define CCA_COUNT_MAX_TX                   6

/* CCA_CHECK_TIME is the time it takes to perform a CCA check. */
/* Note this may be zero. AVRs have 7612 ticks/sec, but block until cca is done */
#define CCA_CHECK_TIME                     RTIMER_ARCH_SECOND / 8192

/* CCA_SLEEP_TIME is the time between two successive CCA checks. */
/* Add 1 when rtimer ticks are coarse */
#if RTIMER_ARCH_SECOND > 8000
#define CCA_SLEEP_TIME                     RTIMER_ARCH_SECOND / 2000
#else
#define CCA_SLEEP_TIME                     (RTIMER_ARCH_SECOND / 2000) + 1
#endif

/* CHECK_TIME is the total time it takes to perform CCA_COUNT_MAX
   CCAs. */
#define CHECK_TIME                         (CCA_COUNT_MAX * (CCA_CHECK_TIME + CCA_SLEEP_TIME))

/* CHECK_TIME_TX is the total time it takes to perform CCA_COUNT_MAX_TX
   CCAs. */
#define CHECK_TIME_TX                      (CCA_COUNT_MAX_TX * (CCA_CHECK_TIME + CCA_SLEEP_TIME))

/* LISTEN_TIME_AFTER_PACKET_DETECTED is the time that we keep checking
   for activity after a potential packet has been detected by a CCA
   check. */
#define LISTEN_TIME_AFTER_PACKET_DETECTED  RTIMER_ARCH_SECOND / 80

/* MAX_SILENCE_PERIODS is the maximum amount of periods (a period is
   CCA_CHECK_TIME + CCA_SLEEP_TIME) that we allow to be silent before
   we turn of the radio. */
#define MAX_SILENCE_PERIODS                5

/* MAX_NONACTIVITY_PERIODS is the maximum number of periods we allow
   the radio to be turned on without any packet being received, when
   WITH_FAST_SLEEP is enabled. */


/* STROBE_TIME is the maximum amount of time a transmitted packet
   should be repeatedly transmitted as part of a transmission. */
#define STROBE_TIME                        (CYCLE_TIME + 2 * CHECK_TIME)

/* GUARD_TIME is the time before the expected phase of a neighbor that
   a transmitted should begin transmitting packets. */
#define GUARD_TIME                         10 * CHECK_TIME + CHECK_TIME_TX

/* INTER_PACKET_INTERVAL is the interval between two successive packet transmissions */
#define INTER_PACKET_INTERVAL              RTIMER_ARCH_SECOND / 5000

/* AFTER_ACK_DETECTECT_WAIT_TIME is the time to wait after a potential
   ACK packet has been detected until we can read it out from the
   radio. */
#define AFTER_ACK_DETECTECT_WAIT_TIME      RTIMER_ARCH_SECOND / 1500

/* MAX_PHASE_STROBE_TIME is the time that we transmit repeated packets
   to a neighbor for which we have a phase lock. */
#define MAX_PHASE_STROBE_TIME              RTIMER_ARCH_SECOND / 60


/* SHORTEST_PACKET_SIZE is the shortest packet that simplerdc
   allows. Packets have to be a certain size to be able to be detected
   by two consecutive CCA checks, and here is where we define this
   shortest size.
   Padded packets will have the wrong ipv6 checksum unless SIMPLERDC_HEADER
   is used (on both sides) and the receiver will ignore them.
   With no header, reduce to transmit a proper multicast RPL DIS. */
#ifdef SIMPLERDC_CONF_SHORTEST_PACKET_SIZE
#define SHORTEST_PACKET_SIZE  SIMPLERDC_CONF_SHORTEST_PACKET_SIZE
#else
#define SHORTEST_PACKET_SIZE               43
#endif


#define ACK_LEN 3

#define DEFAULT_STREAM_TIME (4 * CYCLE_TIME)

#ifndef MIN
#define MIN(a, b) ((a) < (b)? (a) : (b))
#endif /* MIN */
/*---------------------------------------------------------------------------*/
static void
on(void)
{
  if(simplerdc_is_on && radio_is_on == 0) {
    radio_is_on = 1;
    NETSTACK_RADIO.on();
  }
}
/*---------------------------------------------------------------------------*/
static void
off(void)
{
  if(simplerdc_is_on && radio_is_on != 0 &&
     simplerdc_keep_radio_on == 0) {
    radio_is_on = 0;
    NETSTACK_RADIO.off();
  }
}
/*---------------------------------------------------------------------------*/
#if 0
static volatile rtimer_clock_t cycle_start;
static char powercycle(struct rtimer *t, void *ptr);
static void
schedule_powercycle(struct rtimer *t, rtimer_clock_t time)
{
  int r;

  if(simplerdc_is_on) {

    if(RTIMER_CLOCK_LT(RTIMER_TIME(t) + time, RTIMER_NOW() + 2)) {
      time = RTIMER_NOW() - RTIMER_TIME(t) + 2;
    }

    r = rtimer_set(t, RTIMER_TIME(t) + time, 1,
                   (void (*)(struct rtimer *, void *))powercycle, NULL);
    if(r != RTIMER_OK) {
      PRINTF("schedule_powercycle: could not set rtimer\n");
    }
  }
}
#endif /* if 0; commented out code */
/*---------------------------------------------------------------------------*/
#if 0
static void
schedule_powercycle_fixed(struct rtimer *t, rtimer_clock_t fixed_time)
{
  int r;

  if(simplerdc_is_on) {

    if(RTIMER_CLOCK_LT(fixed_time, RTIMER_NOW() + 1)) {
      fixed_time = RTIMER_NOW() + 1;
    }

    r = rtimer_set(t, fixed_time, 1,
                   (void (*)(struct rtimer *, void *))powercycle, NULL);
    if(r != RTIMER_OK) {
      PRINTF("schedule_powercycle: could not set rtimer\n");
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
powercycle_turn_radio_off(void)
{
  if(we_are_sending == 0 && we_are_receiving_burst == 0) {
    off();
  }
}
/*---------------------------------------------------------------------------*/
static void
powercycle_turn_radio_on(void)
{
  if(we_are_sending == 0 && we_are_receiving_burst == 0) {
    on();
  }
}
/*---------------------------------------------------------------------------*/
static char
powercycle(struct rtimer *t, void *ptr)
{
#if SYNC_CYCLE_STARTS
  static volatile rtimer_clock_t sync_cycle_start;
  static volatile uint8_t sync_cycle_phase;
#endif

  PT_BEGIN(&pt);

#if SYNC_CYCLE_STARTS
  sync_cycle_start = RTIMER_NOW();
#else
  cycle_start = RTIMER_NOW();
#endif

  while(1) {
    static uint8_t packet_seen;
    static rtimer_clock_t t0;
    static uint8_t count;

#if SYNC_CYCLE_STARTS
    /* Compute cycle start when RTIMER_ARCH_SECOND is not a multiple of CHANNEL_CHECK_RATE */
    if (sync_cycle_phase++ == NETSTACK_RDC_CHANNEL_CHECK_RATE) {
       sync_cycle_phase = 0;
       sync_cycle_start += RTIMER_ARCH_SECOND;
       cycle_start = sync_cycle_start;
    } else {
#if (RTIMER_ARCH_SECOND * NETSTACK_RDC_CHANNEL_CHECK_RATE) > 65535
       cycle_start = sync_cycle_start + ((unsigned long)(sync_cycle_phase*RTIMER_ARCH_SECOND))/NETSTACK_RDC_CHANNEL_CHECK_RATE;
#else
       cycle_start = sync_cycle_start + (sync_cycle_phase*RTIMER_ARCH_SECOND)/NETSTACK_RDC_CHANNEL_CHECK_RATE;
#endif
    }
#else
    cycle_start += CYCLE_TIME;
#endif

    packet_seen = 0;

    for(count = 0; count < CCA_COUNT_MAX; ++count) {
      t0 = RTIMER_NOW();
      if(we_are_sending == 0 && we_are_receiving_burst == 0) {
        powercycle_turn_radio_on();
        /* Check if a packet is seen in the air. If so, we keep the
             radio on for a while (LISTEN_TIME_AFTER_PACKET_DETECTED) to
             be able to receive the packet. We also continuously check
             the radio medium to make sure that we wasn't woken up by a
             false positive: a spurious radio interference that was not
             caused by an incoming packet. */
        if(NETSTACK_RADIO.channel_clear() == 0) {
          packet_seen = 1;
          break;
        }
        powercycle_turn_radio_off();
      }
      schedule_powercycle_fixed(t, RTIMER_NOW() + CCA_SLEEP_TIME);
      PT_YIELD(&pt);
    }

    if(packet_seen) {
      static rtimer_clock_t start;
      static uint8_t silence_periods, periods;
      start = RTIMER_NOW();

      periods = silence_periods = 0;
      while(we_are_sending == 0 && radio_is_on &&
            RTIMER_CLOCK_LT(RTIMER_NOW(),
                            (start + LISTEN_TIME_AFTER_PACKET_DETECTED))) {

        /* Check for a number of consecutive periods of
             non-activity. If we see two such periods, we turn the
             radio off. Also, if a packet has been successfully
             received (as indicated by the
             NETSTACK_RADIO.pending_packet() function), we stop
             snooping. */
#if !RDC_CONF_HARDWARE_CSMA
       /* A cca cycle will disrupt rx on some radios, e.g. mc1322x, rf230 */
       /*TODO: Modify those drivers to just return the internal RSSI when already in rx mode */
        if(NETSTACK_RADIO.channel_clear()) {
          ++silence_periods;
        } else {
          silence_periods = 0;
        }
#endif

        ++periods;

        if(NETSTACK_RADIO.receiving_packet()) {
          silence_periods = 0;
        }
        if(silence_periods > MAX_SILENCE_PERIODS) {
          powercycle_turn_radio_off();
          break;
        }

        /* XXX originally with WITH_FAST_SLEEP && periods > MAX_NONACTIVITY_PERIODS && ...
         */
        if(!(NETSTACK_RADIO.receiving_packet() || NETSTACK_RADIO.pending_packet())) {
          powercycle_turn_radio_off();
          break;
        }
        if(NETSTACK_RADIO.pending_packet()) {
          break;
        }

        schedule_powercycle(t, CCA_CHECK_TIME + CCA_SLEEP_TIME);
        PT_YIELD(&pt);
      }
      if(radio_is_on) {
        if(!(NETSTACK_RADIO.receiving_packet() ||
             NETSTACK_RADIO.pending_packet()) ||
             !RTIMER_CLOCK_LT(RTIMER_NOW(),
                 (start + LISTEN_TIME_AFTER_PACKET_DETECTED))) {
          powercycle_turn_radio_off();
        }
      }
    }

    if(RTIMER_CLOCK_LT(RTIMER_NOW() - cycle_start, CYCLE_TIME - CHECK_TIME * 4)) {
	     /* Schedule the next powercycle interrupt, or sleep the mcu until then.
                Sleeping will not exit from this interrupt, so ensure an occasional wake cycle
				or foreground processing will be blocked until a packet is detected */
      schedule_powercycle_fixed(t, CYCLE_TIME + cycle_start);
      PT_YIELD(&pt);
    }
  }

  PT_END(&pt);
}
#endif /* if 0; commented out code */
/*---------------------------------------------------------------------------*/
#if 0
static int
send_packet(mac_callback_t mac_callback, void *mac_callback_ptr, struct rdc_buf_list *buf_list)
{
  simplerdc_is_on = simplerdc_was_on;
  we_are_sending = 0;

  /* Determine the return value that we will return from the
     function. We must pass this value to the phase module before we
     return from the function.  */
  if(collisions > 0) {
    ret = MAC_TX_COLLISION;
  } else if(!is_broadcast && !got_strobe_ack) {
    ret = MAC_TX_NOACK;
  } else {
    ret = MAC_TX_OK;
  }


  return ret;
}
#endif /* if 0; commented out code */
/*---------------------------------------------------------------------------*/
/* called with, from 'send one packet', int ret = send_packet(sent, ptr, NULL);*/
static int
send_packet(mac_callback_t mac_callback, void *mac_callback_ptr, struct rdc_buf_list *buf_list)
{
  uint8_t is_broadcast = 0;
  uint8_t is_reliable = 0;
  uint8_t tx_serial;
  clock_time_t end_of_tx;
  int ret;


  PRINTF("SimpleRDC send\n");

  /* sanity checks ---------------------------------------------------------- */
  /* Exit if RDC and radio were explicitly turned off */
  /* XXX allow all off but send anyway?  */
  if (!simplerdc_is_on && !simplerdc_keep_radio_on) {
    PRINTF("simplerdc: radio is turned off\n");
    return MAC_TX_ERR_FATAL;
  }
  /* bad length */
  if(packetbuf_totlen() == 0) {
    PRINTF("simplerdc: send_packet data len 0\n");
    return MAC_TX_ERR_FATAL;
  }

  /* prepare for transmission ----------------------------------------------- */
  packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &rimeaddr_node_addr);

  if(NETSTACK_FRAMER.create() < 0) {
    /* Failed to allocate space for headers */
    PRINTF("simplerdc: send failed, too large header\n");
/*    mac_call_sent_callback(sent, ptr, ret, 1);*/
    return MAC_TX_ERR_FATAL;
  }

  /* let radio copy to tx buffer and do what it needs to do */
  NETSTACK_RADIO.prepare(packetbuf_hdrptr(), packetbuf_totlen());

  /* check to see if broadcast, in which case we won't look for ACKs */
  if(rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER), &rimeaddr_null)) {
    is_broadcast = 1;
    PRINTF("simplerdc: broadcast\n");
  } else {
    is_broadcast = 0;
    PRINTF("simplerdc: unicast to %u.%u\n", packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8[0], packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8[1]);
  }

  if(NETSTACK_RADIO.receiving_packet() || (!is_broadcast && NETSTACK_RADIO.pending_packet())) {
  /*
   * Currently receiving a packet or the radio has packet that needs to be
   * read before sending not-broadcast, as an ACK would overwrite the buffer
   */
    return MAC_TX_COLLISION;
  }

  /* transmit --------------------------------------------------------------- */
  /* make sure the medium is clear */
  if(NETSTACK_RADIO.channel_clear() == 0) {
    return MAC_TX_COLLISION;
  }

  /* transmit the packet repeatedly, and check for ACK if not broadcast */
  end_of_tx = clock_time() + TX_PERIOD;
  while(clock_time() <= end_of_tx) {
    watchdog_periodic();

    /* transmit */
    ret = NETSTACK_RADIO.transmit(packetbuf_totlen());
    if(ret == RADIO_TX_COLLISION) {
      return MAC_TX_COLLISION;
    } else if(ret == RADIO_TX_ERR) {
      return MAC_TX_ERR;
    }

    /* either turn off to save power, or wait for ACK */
    if(is_broadcast) {
      off();
    } else {
      on();
    }
    
    /* wait between transmissions */
    BUSYWAIT_UNTIL(0, BETWEEN_TX_TIME);

    /* if we are hoping for an ACK, check for that here */
    if(!is_broadcast) {
      if(NETSTACK_RADIO.receiving_packet() || NETSTACK_RADIO.pending_packet() ||
            NETSTACK_RADIO.channel_clear() == 0) {
        int len;
        uint8_t ackbuf[ACK_LEN];
        
        if(NETSTACK_RADIO.receiving_packet()) {
          /* wait until any transmissions should be over */
          BUSYWAIT_UNTIL(0, ACK_DETECT_WAIT_TIME);
        }
        
        /* see if it is an ACK to us */
        if(NETSTACK_RADIO.pending_packet()) {
          len = NETSTACK_RADIO.read(ackbuf, ACK_LEN);
          if(len == ACK_LEN && ackbuf[2] == tx_serial) {
            /* ACK received */
            return MAC_TX_OK;
          } else {
            /* Not an ACK or ACK not correct: collision */
            return MAC_TX_COLLISION;
          }
        }

      }
    } /* /checking for ACK between transmissions */
  }   /* /repeated transmissions */
  
  off();

  return MAC_TX_OK;
}
/*---------------------------------------------------------------------------*/
static void
qsend_packet(mac_callback_t sent, void *ptr)
{
  int ret = send_packet(sent, ptr, NULL);
  PRINTF("SimpleRDC qsend\n");
  if(ret != MAC_TX_DEFERRED) {
    mac_call_sent_callback(sent, ptr, ret, 1);
  }
}
/*---------------------------------------------------------------------------*/
static void
qsend_list(mac_callback_t sent, void *ptr, struct rdc_buf_list *buf_list)
{
#if 1
  struct rdc_buf_list *curr = buf_list;
  struct rdc_buf_list *next;
  int ret;
  PRINTF("SimpleRDC qsend_list\n");
  if(curr == NULL) {
    return;
  }
  /* Do not send during reception of a burst */
  if(we_are_receiving_burst) {
    /* Prepare the packetbuf for callback */
    queuebuf_to_packetbuf(curr->buf);
    /* Return COLLISION so the MAC may try again later */
    mac_call_sent_callback(sent, ptr, MAC_TX_COLLISION, 1);
    return;
  }

  /* The receiver needs to be awoken before we send */
  is_receiver_awake = 0;
  do { /* A loop sending a burst of packets from buf_list */
    next = list_item_next(curr);

    /* Prepare the packetbuf */
    queuebuf_to_packetbuf(curr->buf);
    if(next != NULL) {
      packetbuf_set_attr(PACKETBUF_ATTR_PENDING, 1);
    }

    /* Send the current packet */
    ret = send_packet(sent, ptr, curr);
    if(ret != MAC_TX_DEFERRED) {
      mac_call_sent_callback(sent, ptr, ret, 1);
    }

    if(ret == MAC_TX_OK) {
      if(next != NULL) {
        /* We're in a burst, no need to wake the receiver up again */
        is_receiver_awake = 1;
        curr = next;
      }
    } else {
      /* The transmission failed, we stop the burst */
      next = NULL;
    }
  } while(next != NULL);
  is_receiver_awake = 0;
#endif /* if 0; commented out code */
}
/*---------------------------------------------------------------------------*/
/* Timer callback triggered when receiving a burst, after having waited for a next
   packet for a too long time. Turns the radio off and leaves burst reception mode */
static void
recv_burst_off(void *ptr)
{
  off();
  we_are_receiving_burst = 0;
}
/*---------------------------------------------------------------------------*/
static void
input_packet(void)
{
  static struct ctimer ct;
  if(!we_are_receiving_burst) {
    off();
  }

  if(packetbuf_totlen() > 0 && NETSTACK_FRAMER.parse() >= 0) {

    if(packetbuf_datalen() > 0 &&
       packetbuf_totlen() > 0 &&
       (rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER),
                     &rimeaddr_node_addr) ||
        rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER),
                     &rimeaddr_null))) {
      /* This is a regular packet that is destined to us or to the
         broadcast address. */

      /* If FRAME_PENDING is set, we are receiving a packets in a burst */
      // XXX chaged from int to uint8_t
      we_are_receiving_burst = packetbuf_attr(PACKETBUF_ATTR_PENDING);
      if(we_are_receiving_burst) {
        on();
        /* Set a timer to turn the radio off in case we do not receive a next packet */
        ctimer_set(&ct, INTER_PACKET_DEADLINE, recv_burst_off, NULL);
      } else {
        off();
        ctimer_stop(&ct);
      }

      /* Check for duplicate packet by comparing the sequence number
         of the incoming packet with the last few ones we saw. */
      {
        int i;
        for(i = 0; i < MAX_SEQNOS; ++i) {
          if(packetbuf_attr(PACKETBUF_ATTR_PACKET_ID) == received_seqnos[i].seqno &&
             rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_SENDER),
                          &received_seqnos[i].sender)) {
            /* Drop the packet. */
            /*        PRINTF("Drop duplicate simplerdc layer packet\n");*/
            return;
          }
        }
        for(i = MAX_SEQNOS - 1; i > 0; --i) {
          memcpy(&received_seqnos[i], &received_seqnos[i - 1],
                 sizeof(struct seqno));
        }
        received_seqnos[0].seqno = packetbuf_attr(PACKETBUF_ATTR_PACKET_ID);
        rimeaddr_copy(&received_seqnos[0].sender,
                      packetbuf_addr(PACKETBUF_ADDR_SENDER));
      }

      PRINTF("simplerdc: data (%u)\n", packetbuf_datalen());
      NETSTACK_MAC.input();
      return;
    } else {
      PRINTF("simplerdc: data not for us\n");
    }
  } else {
    PRINTF("simplerdc: failed to parse (%u)\n", packetbuf_totlen());
  }
}
/*---------------------------------------------------------------------------*/
// XXX
int8_t
after_on_check(void)
{
  return 0;
}
/*---------------------------------------------------------------------------*/
static struct etimer powercycle_timer;
PROCESS(simplerdc_process, "SimpleRDC process");
PROCESS_THREAD(simplerdc_process, ev, data)
{
  PROCESS_POLLHANDLER();
  PROCESS_EXITHANDLER();
  PROCESS_BEGIN();
  while(1) {
    int8_t chk;
    on();
    etimer_set(&powercycle_timer, SIMPLERDC_ONTIME);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&powercycle_timer));
    
    /* check to see if we can turn off the radio or not here */
    /* the following can happen when we get here:
          nothing was received. We end up here and turn off as check returns 0
          sth was received but CRC failed. The radio never got anything more. Check == 0
          sth was received and CRC OK! Check == len. On receive and CRC OK, then it is automatically off() to avoid overwriting ok buffer.
          we are currently receiving or downloading FIFO, then Check must return -1 or sth to distinguish and this postponed a little while
          we are currently transmitting, Check returns -2;
     */
    chk = after_on_check();   // XXX
    if(chk == 0) {
      // here, we have not received anything during on(), or CRC failed.
      off();
    } else if(chk == -1) {
      // downloading FIFO, don't interrupt it
    } else if(we_are_sending) {
      // currently txing
    } else if(chk > 0) {
      // there is a packet in the buffer, so we send it up
      // NETSTACK_MAC.input();
    }
    
    // check the radio to see that it is in good shape (overflow etc)
    //NETSTACK_RADIO.check();
    etimer_set(&powercycle_timer, SIMPLERDC_OFFTIME);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&powercycle_timer));
  }
  PROCESS_END();
}
 
/*---------------------------------------------------------------------------*/
static void
init(void)
{
  PRINTF("SimpleRDC starting\n");
  radio_is_on = 0;
  process_start(&simplerdc_process, NULL);
/*  PT_INIT(&pt);*/
/*  rtimer_set(&rt, RTIMER_NOW() + CYCLE_TIME, 1, (void (*)(struct rtimer *, void *))powercycle, NULL);*/
  simplerdc_is_on = 1;
}
/*---------------------------------------------------------------------------*/
static int
turn_on(void)
{
  if(simplerdc_is_on == 0) {
    simplerdc_is_on = 1;
    simplerdc_keep_radio_on = 0;
  }
  return 1;
}
/*---------------------------------------------------------------------------*/
static int
turn_off(int keep_radio_on)
{
  simplerdc_is_on = 0;
  if(keep_radio_on > 0) {
    radio_is_on = 1;
    simplerdc_keep_radio_on = 1;
    return NETSTACK_RADIO.on();
  } else {
    radio_is_on = 0;
    simplerdc_keep_radio_on = 0;
    return NETSTACK_RADIO.off();
  }
}
/*---------------------------------------------------------------------------*/
static unsigned short
duty_cycle(void)
{
  return (1ul * CLOCK_SECOND * CYCLE_TIME) / RTIMER_ARCH_SECOND;
}
/*---------------------------------------------------------------------------*/
const struct rdc_driver simplerdc_driver = {
  "SimpleRDC",
  init,
  qsend_packet,
  qsend_list,
  input_packet,
  turn_on,
  turn_off,
  duty_cycle,

#if FOR_REFERENCE_ONLY
/**
 * The structure of a RDC (radio duty cycling) driver in Contiki.
 */
struct rdc_driver {
  char *name;

  /** Initialize the RDC driver */
  void (* init)(void);

  /** Send a packet from the Rime buffer  */
  void (* send)(mac_callback_t sent_callback, void *ptr);

  /** Send a packet list */
  void (* send_list)(mac_callback_t sent_callback, void *ptr, struct rdc_buf_list *list);

  /** Callback for getting notified of incoming packet. */
  void (* input)(void);

  /** Turn the MAC layer on. */
  int (* on)(void);

  /** Turn the MAC layer off. */
  int (* off)(int keep_radio_on);

  /** Returns the channel check interval, expressed in clock_time_t ticks. */
  unsigned short (* channel_check_interval)(void);
};
#endif /* if 0; commented out code */

};
/*---------------------------------------------------------------------------*/
uint16_t
simplerdc_debug_print(void)
{
  return 0;
}
/*---------------------------------------------------------------------------*/