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

/*---------------------------------------------------------------------------*/
#define DEBUG 0
#if DEBUG
#include <stdio.h>
#define PRINTF(...) PRINTF(__VA_ARGS__)
#else
#define PRINTF(...)
#endif
/*---------------------------------------------------------------------------*/
#if 0

Short explanation of SimpleRDC:
  All nodes with SimpleRDC have their radio off and do periodic wake-ups, turning
  on the radio, listening for traffic for a short period of time. To transmit to
  a destination node, we repeatedly transmit the same packet in full until one
  of two things happen:
    1) a full period is covered
    2) for unicasts, we receive an ACK from the destination, meaning it is
       received and sender can go back to sleep early
  The transmissions are timed to the wake-up interval and stay-awake-time so
  that at least two transmissions are performed during a node wake-up. If it
  wakes up and hears traffic, yet do not receive a complete packet within a set
  period, it goes down to sleep again.
  
  ACKs are sent either by the radio hardware automatically, in the radio driver
  or from SimpleRDC. The speed (and hence efficiency) of which this happens is,
  from faster to slower, following the same order.

  SimpleRDC is designed to be lean and simple and stems originally from nullrdc
  and ContikiMAC. To reduce size and complexity, lots of functionality was
  removed, such as support for uIP, neighbor tables to track wake-up phases and
  more.
  
  The radio is assumed to start in Rx mode, and return to Rx after recieving
  or sending data, hence the radio is explicitly turned off when needed to.
  Also, as we assume small packet buffers and simple radio hardware, there is
  no support for streaming data (ie infinite packet length), burst transmissions
  (staying awake to send many packets immediately after eachother).

  SimpleRDC can be easily configured with few settings, as below. Basically,
  what is needed is
    * how often to wake up
    * how long to stay awake at wake up
    * do we send an ACK or the radio driver/hw?
    * how long to wait for an ACK after sending
  The timings are dependant on things like radio bitrate, packet sizes, transfer
  time mcu <-> radio etc. The default settings are optimized for a 250kbps, SPI-
  based radio such as the CC2420 or the CC2500 sending less than 64 byte packets.




  Example power consumption:
    a simple setup of one node simply idle listening and one node sending a
    unicast every third second, the radio duty cycle is as follows.
    Conf
      #define NETSTACK_CONF_MAC     nullmac_driver
      #define NETSTACK_CONF_RDC     simplerdc_driver
      #define NETSTACK_CONF_FRAMER  framer_nullmac
    Without unicast ACKs (so the sender can finish early)
        sender      10.7 % (of which ca 1.4 % is in transmission)
        listener     5.9 %
    With unicast ACKs in SimpleRDC
      COOJA failed to produce statistics strangely enough, but on average the overhead
      for sending should be on average half the sending period, and listener little
      more, thus approximately this:
        sender       7.5 %
        listener     6.5 %
    With unicast ACKs in radio hardware
      this should be even less

  Example code size, the application above
    with contikiMAC, phase optimization etc
       text	   data	    bss	    dec	    hex	filename
      18456	    176	   4912	  23544	   5bf8	dumb.sky
    with simpleRDC
       text	   data	    bss	    dec	    hex	filename
      15892	    160	   4074	  20126	   4e9e	dumb.sky





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
    tBTx    AFTER_TX_HOLDOFF, the time it takes for a packet to be received and
                ACKed.

--------------------------
This is the time it takes to transmit X bytes (total, as seen by radio) at 250 kbaud

    len (B)      t (ms)
    ======       ======
      3           0.20      == length of an ACK
      10          0.42      time to copy to TXFIFO: 30 us
      20          0.74      time to copy to TXFIFO: 52 us
      30          1.06      time to copy to TXFIFO: 73 us
      40          1.38      time to copy to TXFIFO: 95 us
      50          1.70      time to copy to TXFIFO: 116 us
      60          2.02      time to copy to TXFIFO: 138 us

Ie, time to receive a packet, copy ACK->fifo, send ACK, would be < 0.30 ms, but
  do we need to wait for a process to be polled as well? Should be rather fast,
  not the 7.8 ms of a 128 Hz sys clock.

#endif /* if 0; commented out code */
/*---------------------------------------------------------------------------*/

/* ------------------------- SimpleRDC configuration ----------------------- */

/* at what rate to wake up and check for traffic, in Hz, use power of 2 (1,2,4...) */
#define SIMPLERDC_CHECKRATE               8

/*
 * Enable unicast ACKs from SimpleRDC as opposed to the radio layer, or not at all.
 * 
 * if the radio doesn't ACK (either in hardware or at radio layer), then we can
 * ACK here so that a sender can stop sending early. If ACKs are transmitted at
 * neither place, the sender will keep on sending repeatedly for an entire
 * TX_PERIOD time, hence early unicast ACKs are primarily a way to save energy,
 * not (in the current implementation at least) for adding reliability as
 * the sender do not have any mechanism to retransmit at this layer.
 * This ACK will take longer time than at radio layer, so it's less efficient.
 * 
 * Thus, set to 1 if radio layer/hardware does not ACK by itself.
 * Set to 0 if radio handles ACKs, or if you are unsure. It will work, but be
 * less efficient.
 * 
 * Note that for CC2500, you enable radio layer ACKs by setting 
 *    USE_HW_ADDRESS_FILTER             1
 * 
 * On CC2420, radio HW ACKs are supported if packet is framed as an 802.15.4
 * hence in contiki-conf.h:
 *    #define NETSTACK_CONF_FRAMER      framer_802154
 *    #define CC2420_CONF_AUTOACK       1
 * 
 */
#define SIMPLERDC_ACK_UNICAST             0
#if 0
old: /* Radio returns TX_OK/TX_NOACK after autoack wait */
    #ifndef RDC_CONF_HARDWARE_ACK
    #define RDC_CONF_HARDWARE_ACK        0
    #endif
#endif /* if 0; commented out code */


/* timings for wake-up time and transmission ACK-waiting */
#if SIMPLERDC_ACK_UNICAST
  /* SimpleRDC ACKs are slower than radio ACKs, hence relaxed settings */
  #define SIMPLERDC_ONTIME                (CLOCK_SECOND / 64)
  #define BETWEEN_TX_TIME                 ((10ul * RTIMER_SECOND) / 1000) 
#else /* SIMPLERDC_ACK_UNICAST */
  #define SIMPLERDC_ONTIME                (CLOCK_SECOND / 128)
  #define BETWEEN_TX_TIME                 ((2ul * RTIMER_SECOND) / 1000)
#endif /* SIMPLERDC_ACK_UNICAST */

/*
 * if in tx and waiting for an ACK, and we detect traffic, we wait this long to 
 * see if it is indeed an ACK
 */
#define ACK_TX_DETECTED_WAIT_TIME         ((2ul * RTIMER_SECOND)/1000)








/* sleeping time between wake-ups */
#define SIMPLERDC_OFFTIME                 (CLOCK_SECOND / SIMPLERDC_CHECKRATE - SIMPLERDC_ONTIME)

/* for how long to transmit (broadcast, *casts stop at ACK) */
#define TX_PERIOD                         ((CLOCK_SECOND / SIMPLERDC_CHECKRATE) + (2 * SIMPLERDC_ONTIME))




/* Do the radio driver do CSMA and autobackoff */
#if 0
#ifdef RDC_CONF_HARDWARE_CSMA
#if RDC_CONF_HARDWARE_CSMA
#warning "SimpleRDC assumes no hardware CSMA present, does explicit check. Just FYI."
#endif
#endif
#endif /* if 0; commented out code */

/*---------------------------------------------------------------------------*/
/* SimpleRDC header, for ACKs */
struct hdr {
  rimeaddr_t receiver;
  uint8_t seqnr;
};
#define ACK_LEN       3

/* keep a record of the last few received packets, 3 B per sender;
  for duplicate packet detection. */
struct seqno {
  rimeaddr_t sender;
  uint8_t seqno;
};

/* number of sequence numbers to keep track of */
#ifdef NETSTACK_CONF_MAC_SEQNO_HISTORY
#define MAX_SEQNOS NETSTACK_CONF_MAC_SEQNO_HISTORY
#else /* NETSTACK_CONF_MAC_SEQNO_HISTORY */
#define MAX_SEQNOS 2
#endif /* NETSTACK_CONF_MAC_SEQNO_HISTORY */
static struct seqno received_seqnos[MAX_SEQNOS];

/*---------------------------------------------------------------------------*/
static volatile uint8_t simplerdc_is_on = 0;
static volatile uint8_t simplerdc_keep_radio_on = 0;
static volatile uint8_t we_are_sending = 0;
static volatile uint8_t radio_is_on = 0;
/*---------------------------------------------------------------------------*/
#define BUSYWAIT_UNTIL(cond, max_time)                                      \
      do {                                                                  \
        rtimer_clock_t t0;                                                  \
        t0 = RTIMER_NOW();                                                  \
        while(!(cond) && RTIMER_CLOCK_LT(RTIMER_NOW(), t0 + (max_time)));   \
      } while(0)

#ifndef MIN
#define MIN(a, b) ((a) < (b)? (a) : (b))
#endif /* MIN */
/*---------------------------------------------------------------------------*/
/* turn on SimpleRDC */
static void
on(void)
{
  if(simplerdc_is_on && radio_is_on == 0) {
    radio_is_on = 1;
    NETSTACK_RADIO.on();
  }
}
/*---------------------------------------------------------------------------*/
/* turn off SimpleRDC */
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
/* Send a packet with SimpleRDC */
/* called with, from 'send one packet', int ret = send_packet(sent, ptr, NULL);*/
static int
send_packet(mac_callback_t mac_callback, void *mac_callback_ptr, struct rdc_buf_list *buf_list)
{
#if 0

Possible return values

  /**< The MAC layer transmission was OK. */
  MAC_TX_OK,

  /**< The MAC layer transmission could not be performed due to a collision. */
  MAC_TX_COLLISION,

  /**< The MAC layer did not get an acknowledgement for the packet. */
  MAC_TX_NOACK,

  /**< The MAC layer deferred the transmission for a later time. */
  MAC_TX_DEFERRED,

  /**< The MAC layer transmission could not be performed because of an
     error. The upper layer may try again later. */
  MAC_TX_ERR,

  /**< The MAC layer transmission could not be performed because of a
     fatal error. The upper layer does not need to try again, as the
     error will be fatal then as well. */
  MAC_TX_ERR_FATAL,

#endif /* if 0; commented out code */
  uint8_t is_broadcast = 0;
  uint8_t is_reliable = 0;
  int hdrlen;
  clock_time_t end_of_tx;
  int ret;
  struct hdr *chdr;

  // a serial number that is used for duplicate packet detection (->drop)
  static uint8_t tx_serial = 1;

  PRINTF("SimpleRDC send\n");

  /* sanity checks ---------------------------------------------------------- */
  /* Exit if RDC and radio were explicitly turned off */
  /* XXX allow send even though RDC &/| radio is off? */
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

  // is this that packet needs to be ACKed? then yes, for unicasts but that is done automatically so don't use this
  // might be for reliable comms though, but then the primitive itself sets this
/*  packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, 1);*/

//--------------------------------------------------------------- header
  /* simplerdc header, like contikimac header, used to identify packets so they
      can be ACK'ed. Useful for eg unicasts to end a tx-train early and conserve
      some energy */

  /* with simpleRDC header */
  hdrlen = packetbuf_totlen();
  if(packetbuf_hdralloc(sizeof(struct hdr)) == 0) {
    /* Failed to allocate space for contikimac header */
    PRINTF("simplerdc: send failed, too large header\n");
    return MAC_TX_ERR_FATAL;
  }
  chdr = packetbuf_hdrptr();
  chdr->receiver.u8[0] = packetbuf_addr(PACKETBUF_ADDR_SENDER)->u8[0];
  chdr->receiver.u8[1] = packetbuf_addr(PACKETBUF_ADDR_SENDER)->u8[1];
  chdr->seqnr = tx_serial;
  tx_serial++;
  
  /* Create the MAC header for the data packet. */
  hdrlen = NETSTACK_FRAMER.create();
  if(hdrlen < 0) {
    /* Failed to send */
    PRINTF("simplerdc: send failed, too large header\n");
    packetbuf_hdr_remove(sizeof(struct hdr));
    return MAC_TX_ERR_FATAL;
  }
  hdrlen += sizeof(struct hdr);

#if 0
/*old, before header-stuff*/
  if(NETSTACK_FRAMER.create() < 0) {
    /* Failed to allocate space for headers */
    PRINTF("simplerdc: send failed, too large header\n");
    return MAC_TX_ERR_FATAL;
  }
#endif /* if 0; commented out code */


  /* let radio copy to TXFIFO and do what it needs to do */
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
/*    volatile rtimer_clock_t tstart;*/
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
        if(NETSTACK_RADIO.receiving_packet()) {
          /* wait until any transmissions should be over */
/*          BUSYWAIT_UNTIL(0, ACK_TX_DETECTED_WAIT_TIME);*/
          BUSYWAIT_UNTIL(!NETSTACK_RADIO.receiving_packet(), ACK_TX_DETECTED_WAIT_TIME);
        }
        
        /* see if it is an ACK to us */
        if(NETSTACK_RADIO.pending_packet()) {
          uint8_t ab[ACK_LEN];  // ACK-buffer
          uint8_t len;
          len = NETSTACK_RADIO.read(ab, ACK_LEN);
          if(len == ACK_LEN && 
                ab[0] == packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8[0] &&
                ab[1] == packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8[1] &&
                ab[2] == tx_serial) {
            /* ACK received */
            PRINTF("Got ACK!\n");
            return MAC_TX_OK;
          } else {
            /* Not an ACK or ACK not correct: collision, ie someone else is transmitting at the same time */
            return MAC_TX_COLLISION;
          }
        }

      }
    } /* /checking for ACK between transmissions */
  }   /* /repeated transmissions */
  
  off();

  if(is_broadcast) {
    // XXX should this be TX_OK as well? Does it check for this? Semantics....
    return MAC_TX_NOACK;
  }
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
  struct rdc_buf_list *curr = buf_list;
  struct rdc_buf_list *next;
  int ret;

  PRINTF("SimpleRDC qsend_list\n");
  if(curr == NULL) {
    return;
  }

  do {
    /* A loop sending a burst of packets from buf_list */
    next = list_item_next(curr);

    /* Prepare the packetbuf */
    queuebuf_to_packetbuf(curr->buf);

    /* Send the current packet */
    ret = send_packet(sent, ptr, curr);
    if(ret != MAC_TX_DEFERRED) {
      mac_call_sent_callback(sent, ptr, ret, 1);
    }

    if(ret == MAC_TX_OK) {
      if(next != NULL) {
        curr = next;
      }
    } else {
      next = NULL;
    }
  } while(next != NULL);
}
/*---------------------------------------------------------------------------*/
/* read from the radio a received packet */
static void
input_packet(void)
{
  off();
  PRINTF("Input packet\n");

  if(packetbuf_totlen() > 0 && NETSTACK_FRAMER.parse() >= 0) {

    struct hdr *chdr;
    chdr = packetbuf_dataptr();
    PRINTF("simplerdc: got header %u %u %u\n", chdr->receiver.u8[0], chdr->receiver.u8[1], chdr->seqnr);

    packetbuf_hdrreduce(sizeof(struct hdr));
    packetbuf_set_datalen(packetbuf_totlen());    // XXX ???

    if(packetbuf_datalen() > 0 && packetbuf_totlen() > 0 &&
       (rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER),
                     &rimeaddr_node_addr) ||
        rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER),
                     &rimeaddr_null))) {
      /* This is a packet to us or a broadcast */

      {
        /* Check for duplicate packet by comparing the sequence number
           of the incoming packet with the last few ones we saw. */
        uint8_t i;

        for(i = 0; i < MAX_SEQNOS; ++i) {
          /* check if duplicate packet, drop if so */
          if(chdr->receiver.u8[0] == received_seqnos[i].sender.u8[0] &&
             chdr->receiver.u8[1] == received_seqnos[i].sender.u8[1] &&
             chdr->seqnr == received_seqnos[i].seqno) {

            /* Drop the packet. */
            PRINTF("Drop duplicate simplerdc layer packet %u\n", chdr->seqnr);
            return;
          }
        }

        /* new packet, add it to the list of seen packets to avoid getting it again */
        for(i = MAX_SEQNOS - 1; i > 0; --i) {
          memcpy(&received_seqnos[i], &received_seqnos[i - 1], sizeof(struct seqno));
        }
        received_seqnos[0].seqno = chdr->seqnr;
        rimeaddr_copy(&received_seqnos[0].sender, packetbuf_addr(PACKETBUF_ADDR_SENDER));

#if SIMPLERDC_ACK_UNICAST
        /* respond to unicasts with ACK so the sender can finish early */
        if(rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER), &rimeaddr_node_addr)) {
          uint8_t ab[ACK_LEN];  // ACK-buffer
          ab[0] = rimeaddr_node_addr.u8[0];
          ab[1] = rimeaddr_node_addr.u8[1];
          ab[2] = chdr->seqnr;
          NETSTACK_RADIO.send(ab, ACK_LEN);
        }
#endif /* SIMPLERDC_ACK_UNICAST */
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
/* do all necessary checks after the radio has been on, to see if we can turn
  it off again */
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
/*      while(after_on_check() == -1) {*/
/*        PROCESS_PAUSE();*/
/*      }*/
    } else if(we_are_sending) {
      // currently txing
    } else if(chk > 0) {
      // there is a packet in the buffer, and we don't touch it; sent up from elsewhere
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
channel_check_interval(void)
{
  return CLOCK_SECOND / SIMPLERDC_CHECKRATE;
}
/*---------------------------------------------------------------------------*/
const struct rdc_driver simplerdc_driver = {
  "SimpleRDC",
  /** Initialize the RDC driver */
  init,
  /** Send a packet from the Rime buffer  */
  qsend_packet,
  /** Send a packet list */
  qsend_list,
  /** Callback for getting notified of incoming packet. */
  input_packet,
  /** Turn the MAC layer on. */
  turn_on,
  /** Turn the MAC layer off. */
  turn_off,
  /** Returns the channel check interval, expressed in clock_time_t ticks. */
  channel_check_interval,

#if FOR_REFERENCE_ONLY_DONT_SET_TRUE
this is just here for reference, this will never be true
/* The structure of a RDC (radio duty cycling) driver in Contiki. */
struct rdc_driver {
  char *name;
  void (* init)(void);
  void (* send)(mac_callback_t sent_callback, void *ptr);
  void (* send_list)(mac_callback_t sent_callback, void *ptr, struct rdc_buf_list *list);
  void (* input)(void);
  int (* on)(void);
  int (* off)(int keep_radio_on);
  unsigned short (* channel_check_interval)(void);
};
#endif /* if 0; commented out code */
};
/*---------------------------------------------------------------------------*/
/*uint16_t*/
/*simplerdc_debug_print(void)*/
/*{*/
/*  return 0;*/
/*}*/
/*---------------------------------------------------------------------------*/
