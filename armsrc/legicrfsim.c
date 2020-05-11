//-----------------------------------------------------------------------------
// (c) 2009 Henryk Plötz <henryk@ploetzli.ch>
//     2016 Iceman
//     2018 AntiCat
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// LEGIC RF simulation code
//-----------------------------------------------------------------------------
#include "legicrfsim.h"
#include "legicrf.h"

#include "crc.h"                /* legic crc-4 */
#include "legic_prng.h"         /* legic PRNG impl */
#include "legic.h"              /* legic_card_select_t struct */

#include "proxmark3_arm.h"
#include "BigBuf.h"
#include "fpgaloader.h"
#include "ticks.h"
#include "dbprint.h"
#include "util.h"

static uint8_t *legic_mem;      /* card memory, used for sim */
static legic_card_select_t card;/* metadata of currently selected card */
static crc_t legic_crc;

//-----------------------------------------------------------------------------
// Frame timing and pseudorandom number generator
//
// The Prng is forwarded every 99.1us (TAG_BIT_PERIOD), except when the reader is
// transmitting. In that case the prng has to be forwarded every bit transmitted:
//  - 31.3us for a 0 (RWD_TIME_0)
//  - 99.1us for a 1 (RWD_TIME_1)
//
// The data dependent timing makes writing comprehensible code significantly
// harder. The current aproach forwards the prng data based if there is data on
// air and time based, using GetCountSspClk(), during computational and wait
// periodes. SSP Clock is clocked by the FPGA at 212 kHz (subcarrier frequency).
//
// To not have the necessity to calculate/guess exection time dependend timeouts
// tx_frame and rx_frame use a shared timestamp to coordinate tx and rx timeslots.
//-----------------------------------------------------------------------------

static uint32_t last_frame_end; /* ts of last bit of previews rx or tx frame */

#define TAG_FRAME_WAIT       70 /* 330us from READER frame end to TAG frame start */
#define TAG_ACK_WAIT        758 /* 3.57ms from READER frame end to TAG write ACK */
#define TAG_BIT_PERIOD       21 /* 99.1us */

#define RWD_TIME_PAUSE        4 /* 18.9us */
#define RWD_TIME_1           21 /* RWD_TIME_PAUSE 18.9us off + 80.2us on = 99.1us */
#define RWD_TIME_0           13 /* RWD_TIME_PAUSE 18.9us off + 42.4us on = 61.3us */
#define RWD_CMD_TIMEOUT     120 /* 120 * 99.1us (arbitrary value) */
#define RWD_MIN_FRAME_LEN     6 /* Shortest frame is 6 bits */
#define RWD_MAX_FRAME_LEN    23 /* Longest frame is 23 bits */

#define RWD_PULSE             1 /* Pulse is signaled with GPIO_SSC_DIN high */
#define RWD_PAUSE             0 /* Pause is signaled with GPIO_SSC_DIN low */

//-----------------------------------------------------------------------------
// Demodulation
//-----------------------------------------------------------------------------

// Returns true if a pulse/pause is received within timeout
// Note: inlining this function would fail with -Os
#ifdef __OPTIMIZE_SIZE__
static bool wait_for(bool value, const uint32_t timeout) {
#else
static inline bool wait_for(bool value, const uint32_t timeout) {
#endif
    while ((bool)(AT91C_BASE_PIOA->PIO_PDSR & GPIO_SSC_DIN) != value) {
        if (GetCountSspClk() > timeout) {
            return false;
        }
    }
    return true;
}

// Returns a demedulated bit or -1 on code violation
//
// rx_bit decodes bits using a thresholds. rx_bit has to be called by as soon as
// a frame starts (first pause is received). rx_bit checks for a pause up to
// 18.9us followed by a pulse of 80.2us or 42.4us:
//  - A bit length <18.9us is a code violation
//  - A bit length >80.2us is a 1
//  - A bit length <80.2us is a 0
//  - A bit length >148.6us is a code violation
static inline int8_t rx_bit(void) {
    // backup ts for threshold calculation
    uint32_t bit_start = last_frame_end;

    // wait for pause to end
    if (!wait_for(RWD_PULSE, bit_start + RWD_TIME_1 * 3 / 2)) {
        return -1;
    }

    // wait for next pause
    if (!wait_for(RWD_PAUSE, bit_start + RWD_TIME_1 * 3 / 2)) {
        return -1;
    }

    // update bit and frame end
    last_frame_end = GetCountSspClk();

    // check for code violation (bit to short)
    if (last_frame_end - bit_start < RWD_TIME_PAUSE) {
        return -1;
    }

    // apply threshold (average of RWD_TIME_0 and )
    return (last_frame_end - bit_start > (RWD_TIME_0 + RWD_TIME_1) / 2);
}

//-----------------------------------------------------------------------------
// Modulation
//
// LEGIC RF uses a very basic load modulation from card to reader:
//  - Subcarrier on for a 1
//  - Subcarrier off for for a 0
//
// The 212kHz subcarrier is generated by the FPGA as well as a mathcing ssp clk.
// Each bit is transfered in a 99.1us slot and the first timeslot starts 330us
// after the final 20us pause generated by the reader.
//-----------------------------------------------------------------------------

// Transmits a bit
//
// Note: The Subcarrier is not disabled during bits to prevent glitches. This is
//       not mandatory but results in a cleaner signal. tx_frame will disable
//       the subcarrier when the frame is done.
// Note: inlining this function would fail with -Os
#ifdef __OPTIMIZE_SIZE__
static void tx_bit(bool bit) {
#else
static inline void tx_bit(bool bit) {
#endif
    LED_C_ON();

    if (bit) {
        // modulate subcarrier
        HIGH(GPIO_SSC_DOUT);
    } else {
        // do not modulate subcarrier
        LOW(GPIO_SSC_DOUT);
    }

    // wait for tx timeslot to end
    last_frame_end += TAG_BIT_PERIOD;
    while (GetCountSspClk() < last_frame_end) { };
    LED_C_OFF();
}

//-----------------------------------------------------------------------------
// Frame Handling
//
// The LEGIC RF protocol from reader to card does not include explicit frame
// start/stop information or length information. The tag detects end of frame
// trough an extended pulse (>99.1us) without a pause.
// In reverse direction (card to reader) the number of bites is well known
// and depends only the command received (IV, ACK, READ or WRITE).
//-----------------------------------------------------------------------------

static void tx_frame(uint32_t frame, uint8_t len) {
    // wait for next tx timeslot
    last_frame_end += TAG_FRAME_WAIT;
    legic_prng_forward(TAG_FRAME_WAIT / TAG_BIT_PERIOD - 1);
    while (GetCountSspClk() < last_frame_end) { };

    // backup ts for trace log
    uint32_t last_frame_start = last_frame_end;

    // transmit frame, MSB first
    for (uint8_t i = 0; i < len; ++i) {
        bool bit = (frame >> i) & 0x01;
        tx_bit(bit ^ legic_prng_get_bit());
        legic_prng_forward(1);
    };

    // disable subcarrier
    LOW(GPIO_SSC_DOUT);

    // log
    uint8_t cmdbytes[] = {len, BYTEx(frame, 0), BYTEx(frame, 1)};
    LogTrace(cmdbytes, sizeof(cmdbytes), last_frame_start, last_frame_end, NULL, false);
}

static void tx_ack(void) {
    // wait for ack timeslot
    last_frame_end += TAG_ACK_WAIT;
    legic_prng_forward(TAG_ACK_WAIT / TAG_BIT_PERIOD - 1);
    while (GetCountSspClk() < last_frame_end) { };

    // backup ts for trace log
    uint32_t last_frame_start = last_frame_end;

    // transmit ack (ack is not encrypted)
    tx_bit(true);
    legic_prng_forward(1);

    // disable subcarrier
    LOW(GPIO_SSC_DOUT);

    // log
    uint8_t cmdbytes[] = {1, 1};
    LogTrace(cmdbytes, sizeof(cmdbytes), last_frame_start, last_frame_end, NULL, false);
}

// Returns a demedulated frame or -1 on code violation
//
// Since TX to RX delay is arbitrary rx_frame has to:
//  - detect start of frame (first pause)
//  - forward prng based on ts/TAG_BIT_PERIOD
//  - receive the frame
//  - detect end of frame (last pause)
static int32_t rx_frame(uint8_t *len) {
    int32_t frame = 0;

    // add 2 SSP clock cycles (1 for tx and 1 for rx pipeline delay)
    // those will be substracted at the end of the rx phase
    last_frame_end -= 2;

    // wait for first pause (start of frame)
    for (uint8_t i = 0; true; ++i) {
        // increment prng every TAG_BIT_PERIOD
        last_frame_end += TAG_BIT_PERIOD;
        legic_prng_forward(1);

        // if start of frame was received exit delay loop
        if (wait_for(RWD_PAUSE, last_frame_end)) {
            last_frame_end = GetCountSspClk();
            break;
        }

        // check for code violation
        if (i > RWD_CMD_TIMEOUT) {
            return -1;
        }
    }

    // backup ts for trace log
    uint32_t last_frame_start = last_frame_end;

    // receive frame
    for (*len = 0; true; ++(*len)) {
        // receive next bit
        LED_B_ON();
        int8_t bit = rx_bit();
        LED_B_OFF();

        // check for code violation and to short / long frame
        if ((bit < 0) && ((*len < RWD_MIN_FRAME_LEN) || (*len > RWD_MAX_FRAME_LEN))) {
            return -1;
        }

        // check for code violation caused by end of frame
        if (bit < 0) {
            break;
        }

        // append bit
        frame |= (bit ^ legic_prng_get_bit()) << (*len);
        legic_prng_forward(1);
    }

    // rx_bit sets coordination timestamp to start of pause, append pause duration
    // and substract 2 SSP clock cycles (1 for rx and 1 for tx pipeline delay) to
    // obtain exact end of frame.
    last_frame_end += RWD_TIME_PAUSE - 2;

    // log
    uint8_t cmdbytes[] = {*len, BYTEx(frame, 0), BYTEx(frame, 1), BYTEx(frame, 2)};
    LogTrace(cmdbytes, sizeof(cmdbytes), last_frame_start, last_frame_end, NULL, true);
    return frame;
}

//-----------------------------------------------------------------------------
// Legic Simulator
//-----------------------------------------------------------------------------

static int32_t init_card(uint8_t cardtype, legic_card_select_t *p_card) {
    p_card->tagtype = cardtype;

    switch (p_card->tagtype) {
        case 0:
            p_card->cmdsize = 6;
            p_card->addrsize = 5;
            p_card->cardsize = 22;
            break;
        case 1:
            p_card->cmdsize = 9;
            p_card->addrsize = 8;
            p_card->cardsize = 256;
            break;
        case 2:
            p_card->cmdsize = 11;
            p_card->addrsize = 10;
            p_card->cardsize = 1024;
            break;
        default:
            p_card->cmdsize = 0;
            p_card->addrsize = 0;
            p_card->cardsize = 0;
            return 2;
    }
    return 0;
}

static void init_tag(void) {
    // configure FPGA
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR
                      | FPGA_HF_SIMULATOR_MODULATE_212K);
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // configure SSC with defaults
    FpgaSetupSsc();

    // first pull output to low to prevent glitches then re-claim GPIO_SSC_DOUT
    LOW(GPIO_SSC_DOUT);
    AT91C_BASE_PIOA->PIO_OER = GPIO_SSC_DOUT;
    AT91C_BASE_PIOA->PIO_PER = GPIO_SSC_DOUT;

    // reserve a cardmem, meaning we can use the tracelog function in bigbuff easier.
    legic_mem = BigBuf_get_EM_addr();

    // start trace
    clear_trace();
    set_tracing(true);

    // init crc calculator
    crc_init(&legic_crc, 4, 0x19 >> 1, 0x05, 0);

    // start 212kHz timer (running from SSP Clock)
    StartCountSspClk();
}

// Setup reader to card connection
//
// The setup consists of a three way handshake:
//  - Receive initialisation vector 7 bits
//  - Transmit card type 6 bits
//  - Receive Acknowledge 6 bits
static int32_t setup_phase(legic_card_select_t *p_card) {
    uint8_t len = 0;

    // init coordination timestamp
    last_frame_end = GetCountSspClk();

    // reset prng
    legic_prng_init(0);

    // wait for iv
    int32_t iv = rx_frame(&len);
    if ((len != 7) || (iv < 0)) {
        return -1;
    }

    // configure prng
    legic_prng_init(iv);

    // reply with card type
    switch (p_card->tagtype) {
        case 0:
            tx_frame(0x0D, 6);
            break;
        case 1:
            tx_frame(0x1D, 6);
            break;
        case 2:
            tx_frame(0x3D, 6);
            break;
    }

    // wait for ack
    int32_t ack = rx_frame(&len);
    if ((len != 6) || (ack < 0)) {
        return -1;
    }

    // validate data
    switch (p_card->tagtype) {
        case 0:
            if (ack != 0x19) return -1;
            break;
        case 1:
            if (ack != 0x39) return -1;
            break;
        case 2:
            if (ack != 0x39) return -1;
            break;
    }

    // During rx the prng is clocked using the variable reader period.
    // Since rx_frame detects end of frame by detecting a code violation,
    // the prng is off by one bit period after each rx phase. Hence, tx
    // code advances the prng by (TAG_FRAME_WAIT/TAG_BIT_PERIOD - 1).
    // This is not possible for back to back rx, so this quirk reduces
    // the gap by one period.
    last_frame_end += TAG_BIT_PERIOD;

    return 0;
}

static uint8_t calc_crc4(uint16_t cmd, uint8_t cmd_sz, uint8_t value) {
    crc_clear(&legic_crc);
    crc_update(&legic_crc, (value << cmd_sz) | cmd, 8 + cmd_sz);
    return crc_finish(&legic_crc);
}

static int32_t connected_phase(legic_card_select_t *p_card) {
    uint8_t len = 0;

    // wait for command
    int32_t cmd = rx_frame(&len);
    if (cmd < 0) {
        return -1;
    }

    // check if command is LEGIC_READ
    if (len == p_card->cmdsize) {
        // prepare data
        uint8_t byte = legic_mem[cmd >> 1];
        uint8_t crc = calc_crc4(cmd, p_card->cmdsize, byte);

        // transmit data
        tx_frame((crc << 8) | byte, 12);

        return 0;
    }

    // check if command is LEGIC_WRITE
    if (len == p_card->cmdsize + 8 + 4) {
        // decode data
        uint16_t mask = (1 << p_card->addrsize) - 1;
        uint16_t addr = (cmd >> 1) & mask;
        uint8_t  byte = (cmd >> p_card->cmdsize) & 0xff;
        uint8_t  crc  = (cmd >> (p_card->cmdsize + 8)) & 0xf;

        // check received against calculated crc
        uint8_t calc_crc = calc_crc4(addr << 1, p_card->cmdsize, byte);
        if (calc_crc != crc) {
            Dbprintf("!!! crc mismatch: %x != %x !!!",  calc_crc, crc);
            return -1;
        }

        // store data
        legic_mem[addr] = byte;

        // transmit ack
        tx_ack();

        return 0;
    }

    return -1;
}

//-----------------------------------------------------------------------------
// Command Line Interface
//
// Only this function is public / called from appmain.c
//-----------------------------------------------------------------------------

void LegicRfSimulate(uint8_t cardtype) {
    // configure ARM and FPGA
    init_tag();

    // verify command line input
    if (init_card(cardtype, &card) != 0) {
        DbpString("[!] Unknown tagtype.");
        goto OUT;
    }

    LED_A_ON();
    DbpString("[=] Starting Legic emulator, press " _YELLOW_("button") " to end");
    while (!BUTTON_PRESS() && !data_available()) {
        WDT_HIT();

        // wait for carrier, restart after timeout
        if (!wait_for(RWD_PULSE, GetCountSspClk() + TAG_BIT_PERIOD)) {
            continue;
        }

        // wait for connection, restart on error
        if (setup_phase(&card)) {
            continue;
        }

        // conection is established, process commands until one fails
        while (!connected_phase(&card)) {
            WDT_HIT();
        }
    }

OUT:
    DbpString("[=] Sim stopped");
    switch_off();
    StopTicks();
}
