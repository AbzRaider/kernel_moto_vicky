/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#ifndef PANEL_NT36672C_FHDP_DSI_VDO_120HZ_TIANMA_HFP
#define PANEL_NT36672C_FHDP_DSI_VDO_120HZ_TIANMA_HFP


#define REGFLAG_DELAY           0xFFFC
#define REGFLAG_UDELAY          0xFFFB
#define REGFLAG_END_OF_TABLE    0xFFFD
#define REGFLAG_RESET_LOW       0xFFFE
#define REGFLAG_RESET_HIGH      0xFFFF

#define FRAME_WIDTH                 1080
#define FRAME_HEIGHT                2400

#define PHYSICAL_WIDTH              67716
#define PHYSICAL_HEIGHT            150480

#define DATA_RATE                   1000
#define HSA                         4//16
#define HBP                         40//52
#define VSA                         2//4
#define VBP                         20

#define DISP_CLK                    356552 // 356552//176037  //H total *V total * V freq = 1201*2474*120=356552
#define DISP_PLL_CLK                500

/*Parameter setting for mode 0 Start*/
#define MODE_60_FPS                  60
#define MODE_60_VFP                  2528
#define MODE_60_HFP                  77
#define MODE_60_DATA_RATE            1000
/*Parameter setting for mode 0 End*/

/*Parameter setting for mode 30 Start*/
#define MODE_30_FPS                  30
#define MODE_30_VFP                  7476
#define MODE_30_HFP                  77
#define MODE_30_DATA_RATE            1000
/*Parameter setting for mode 30 End*/

/*Parameter setting for mode 1 Start*/
#define MODE_90_FPS                  90
#define MODE_90_VFP                  879
#define MODE_90_HFP                  77
#define MODE_90_DATA_RATE            1000
/*Parameter setting for mode 1 End*/

/*Parameter setting for mode 2 Start*/
#define MODE_120_FPS                  120
#define MODE_120_VFP                  54
#define MODE_120_HFP                  77
#define MODE_120_DATA_RATE            1000
/*Parameter setting for mode 2 End*/

#define LFR_EN                      0
/* DSC RELATED */

#define DSC_ENABLE                  1
#define DSC_VER                     17
#define DSC_SLICE_MODE              1
#define DSC_RGB_SWAP                0
#define DSC_DSC_CFG                 34
#define DSC_RCT_ON                  1
#define DSC_BIT_PER_CHANNEL         8
#define DSC_DSC_LINE_BUF_DEPTH      9
#define DSC_BP_ENABLE               1
#define DSC_BIT_PER_PIXEL           128
//define DSC_PIC_HEIGHT
//define DSC_PIC_WIDTH
#define DSC_SLICE_HEIGHT            8
#define DSC_SLICE_WIDTH             540
#define DSC_CHUNK_SIZE              540
#define DSC_XMIT_DELAY              170
#define DSC_DEC_DELAY               526
#define DSC_SCALE_VALUE             32
#define DSC_INCREMENT_INTERVAL      43
#define DSC_DECREMENT_INTERVAL      7
#define DSC_LINE_BPG_OFFSET         12
#define DSC_NFL_BPG_OFFSET          3511
#define DSC_SLICE_BPG_OFFSET        3255
#define DSC_INITIAL_OFFSET          6144
#define DSC_FINAL_OFFSET            7072
#define DSC_FLATNESS_MINQP          3
#define DSC_FLATNESS_MAXQP          12
#define DSC_RC_MODEL_SIZE           8192
#define DSC_RC_EDGE_FACTOR          6
#define DSC_RC_QUANT_INCR_LIMIT0    11
#define DSC_RC_QUANT_INCR_LIMIT1    11
#define DSC_RC_TGT_OFFSET_HI        3
#define DSC_RC_TGT_OFFSET_LO        3

#endif //end of PANEL_NT36672C_FHDP_DSI_VDO_120HZ_TIANMA_HFP
