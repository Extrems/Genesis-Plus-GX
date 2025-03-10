/****************************************************************************
 *  gx_video.c
 *
 *  Genesis Plus GX video & rendering support
 *
 *  Copyright Eke-Eke (2007-2023), based on original work from Softdev (2006)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************/

#include "shared.h"
#include "font.h"
#include "md_ntsc.h"
#include "sms_ntsc.h"
#include "gx_input.h"

#include <ogc/lwp_watchdog.h>
#include <png.h>

/* libpng wrapper */
typedef struct
{
  u8 *buffer;
  u32 offset;
} png_image_desc;

#include "Crosshair_p1_png.h"
#include "Crosshair_p2_png.h"
#include "CD_access_off_png.h"
#include "CD_access_on_png.h"
#include "CD_ready_off_png.h"
#include "CD_ready_on_png.h"

/*** VI Mode ***/
GXRModeObj *vmode;

/*** 50/60hz flag ***/
u32 gc_pal;

/*** NTSC Filters ***/
sms_ntsc_t *sms_ntsc;
md_ntsc_t *md_ntsc;

/*** GX FIFO ***/
static u8 gp_fifo[GX_FIFO_MINSIZE] ATTRIBUTE_ALIGN(32);

/*** GX Textures ***/
static u32 vwidth, vheight;
static gx_texture *crosshair[2];
static gx_texture *cd_leds[2][2];
static GXTexObj screenTexObj;

/*** Framebuffers ***/
static u32 *xfb[2];
static u8 fbCurrent;

/*** Frame Sync ***/
u32 videoSync;
static u32 videoWait;
static u32 frameCount;
static u64 starttime;

/*** OSD ***/
static u32 osd;
static char msg[16];

/***************************************************************************************/
/*   Emulation video modes                                                             */
/***************************************************************************************/
static GXRModeObj *rmode;

/* 288 lines progressive (PAL 50Hz) */
static GXRModeObj TV50hz_288p = 
{
  VI_TVMODE_PAL_DS,             // viDisplayMode
  640,                          // fbWidth
  VI_MAX_HEIGHT_PAL/2,          // efbHeight
  VI_MAX_HEIGHT_PAL/2,          // xfbHeight
  0,                            // viXOrigin
  0,                            // viYOrigin
  VI_MAX_WIDTH_PAL,             // viWidth
  VI_MAX_HEIGHT_PAL,            // viHeight
  VI_XFBMODE_SF,                // xFBmode
  GX_FALSE,                     // field_rendering
  GX_FALSE,                     // aa

  // sample points arranged in increasing Y order
  {
    {6,6},{6,6},{6,6},  // pix 0, 3 sample points, 1/12 units, 4 bits each
    {6,6},{6,6},{6,6},  // pix 1
    {6,6},{6,6},{6,6},  // pix 2
    {6,6},{6,6},{6,6}   // pix 3
  },

  // vertical filter[7], 1/64 units, 6 bits each
  {
    0,         // line n-1
    0,         // line n-1
    21,        // line n
    22,        // line n
    21,        // line n
    0,         // line n+1
    0          // line n+1
  }
};

/* 288 lines interlaced (PAL 50Hz) */
static GXRModeObj TV50hz_288i = 
{
  VI_TVMODE_PAL_INT,            // viDisplayMode
  640,                          // fbWidth
  VI_MAX_HEIGHT_PAL/2,          // efbHeight
  VI_MAX_HEIGHT_PAL/2,          // xfbHeight
  0,                            // viXOrigin
  0,                            // viYOrigin
  VI_MAX_WIDTH_PAL,             // viWidth
  VI_MAX_HEIGHT_PAL,            // viHeight
  VI_XFBMODE_SF,                // xFBmode
  GX_TRUE,                      // field_rendering
  GX_FALSE,                     // aa

  // sample points arranged in increasing Y order
  {
    {6,6},{6,6},{6,6},  // pix 0, 3 sample points, 1/12 units, 4 bits each
    {6,6},{6,6},{6,6},  // pix 1
    {6,6},{6,6},{6,6},  // pix 2
    {6,6},{6,6},{6,6}   // pix 3
  },

  // vertical filter[7], 1/64 units, 6 bits each
  {
    0,         // line n-1
    0,         // line n-1
    21,        // line n
    22,        // line n
    21,        // line n
    0,         // line n+1
    0          // line n+1
  }
};

/* 576 lines interlaced (PAL 50Hz, scaled) */
static GXRModeObj TV50hz_576i = 
{
  VI_TVMODE_PAL_INT,  // viDisplayMode
  640,                // fbWidth
  480,                // efbHeight
  VI_MAX_HEIGHT_PAL,  // xfbHeight
  0,                  // viXOrigin
  0,                  // viYOrigin
  VI_MAX_WIDTH_PAL,   // viWidth
  VI_MAX_HEIGHT_PAL,  // viHeight
  VI_XFBMODE_DF,      // xFBmode
  GX_FALSE,           // field_rendering
  GX_FALSE,           // aa

  // sample points arranged in increasing Y order
  {
    {6,6},{6,6},{6,6},  // pix 0, 3 sample points, 1/12 units, 4 bits each
    {6,6},{6,6},{6,6},  // pix 1
    {6,6},{6,6},{6,6},  // pix 2
    {6,6},{6,6},{6,6}   // pix 3
  },

  // vertical filter[7], 1/64 units, 6 bits each
  {
    8,         // line n-1
    8,         // line n-1
    10,        // line n
    12,        // line n
    10,        // line n
    8,         // line n+1
    8          // line n+1
  }
};

/* 240 lines progressive (NTSC or PAL 60Hz) */
static GXRModeObj TV60hz_240p = 
{
  VI_TVMODE_EURGB60_DS, // viDisplayMode
  640,                  // fbWidth
  VI_MAX_HEIGHT_NTSC/2, // efbHeight
  VI_MAX_HEIGHT_NTSC/2, // xfbHeight
  0,                    // viXOrigin
  0,                    // viYOrigin
  VI_MAX_WIDTH_NTSC,    // viWidth
  VI_MAX_HEIGHT_NTSC,   // viHeight
  VI_XFBMODE_SF,        // xFBmode
  GX_FALSE,             // field_rendering
  GX_FALSE,             // aa

  // sample points arranged in increasing Y order
  {
    {6,6},{6,6},{6,6},  // pix 0, 3 sample points, 1/12 units, 4 bits each
    {6,6},{6,6},{6,6},  // pix 1
    {6,6},{6,6},{6,6},  // pix 2
    {6,6},{6,6},{6,6}   // pix 3
  },

  // vertical filter[7], 1/64 units, 6 bits each
  {
    0,         // line n-1
    0,         // line n-1
    21,        // line n
    22,        // line n
    21,        // line n
    0,         // line n+1
    0          // line n+1
  }
};

/* 240 lines interlaced (NTSC or PAL 60Hz) */
static GXRModeObj TV60hz_240i = 
{
    VI_TVMODE_EURGB60_INT,  // viDisplayMode
    640,                    // fbWidth
    VI_MAX_HEIGHT_NTSC/2,   // efbHeight
    VI_MAX_HEIGHT_NTSC/2,   // xfbHeight
    0,                      // viXOrigin
    0,                      // viYOrigin
    VI_MAX_WIDTH_NTSC,      // viWidth
    VI_MAX_HEIGHT_NTSC,     // viHeight
    VI_XFBMODE_SF,          // xFBmode
    GX_TRUE,                // field_rendering
    GX_FALSE,               // aa

  // sample points arranged in increasing Y order
  {
    {3,2},{9,6},{3,10},  // pix 0, 3 sample points, 1/12 units, 4 bits each
    {3,2},{9,6},{3,10},  // pix 1
    {9,2},{3,6},{9,10},  // pix 2
    {9,2},{3,6},{9,10}   // pix 3
  },

  // vertical filter[7], 1/64 units, 6 bits each
  {
    0,         // line n-1
    0,         // line n-1
    21,        // line n
    22,        // line n
    21,        // line n
    0,         // line n+1
    0          // line n+1
  }
};

/* 480 lines interlaced (NTSC or PAL 60Hz) */
static GXRModeObj TV60hz_480i = 
{
  VI_TVMODE_EURGB60_INT,// viDisplayMode
  640,                  // fbWidth
  VI_MAX_HEIGHT_NTSC,   // efbHeight
  VI_MAX_HEIGHT_NTSC,   // xfbHeight
  0,                    // viXOrigin
  0,                    // viYOrigin
  VI_MAX_WIDTH_NTSC,    // viWidth
  VI_MAX_HEIGHT_NTSC,   // viHeight
  VI_XFBMODE_DF,        // xFBmode
  GX_FALSE,             // field_rendering
  GX_FALSE,             // aa

  // sample points arranged in increasing Y order
  {
    {6,6},{6,6},{6,6},  // pix 0, 3 sample points, 1/12 units, 4 bits each
    {6,6},{6,6},{6,6},  // pix 1
    {6,6},{6,6},{6,6},  // pix 2
    {6,6},{6,6},{6,6}   // pix 3
  },

  // vertical filter[7], 1/64 units, 6 bits each
  {
    8,         // line n-1
    8,         // line n-1
    10,        // line n
    12,        // line n
    10,        // line n
    8,         // line n+1
    8          // line n+1
  }
};

/* TV modes pointer table */
static GXRModeObj *tvmodes[6] =
{
   /* 60hz modes */
   &TV60hz_240p,
   &TV60hz_240i,
   &TV60hz_480i,

   /* 50Hz modes */
   &TV50hz_288p,
   &TV50hz_288i,
   &TV50hz_576i   
};

/***************************************************************************************/
/*   GX rendering engine                                                               */
/***************************************************************************************/

typedef struct tagcamera
{
  guVector pos;
  guVector up;
  guVector view;
} camera;

static s16 square[8] ATTRIBUTE_ALIGN (32);

static camera cam =
{
  {0.0F, 0.0F, -100.0F},
  {0.0F, -1.0F, 0.0F},
  {0.0F, 0.0F, 0.0F}
};

/*** GX Display List ***/
static u8 screenDisplayList[32] ATTRIBUTE_ALIGN(32) =
{
  GX_QUADS | GX_VTXFMT0,        /* textured quad rendering (Vertex Format 0) */
  0x00, 0x04,                   /* one quad = 4x vertex */
  0x03, 0x00, 0x00, 0x00, 0x00, /* top left corner */
  0x02, 0x00, 0x01, 0x00, 0x00, /* top right corner */
  0x01, 0x00, 0x01, 0x00, 0x01, /* bottom right corner */
  0x00, 0x00, 0x00, 0x00, 0x01, /* bottom left corner */
  0x00, 0x00, 0x00, 0x00, 0x00, /* padding */
  0x00, 0x00, 0x00, 0x00
};

/* VSYNC callback */
static void vi_callback(u32 cnt)
{
  /* get audio DMA remaining length */
  u16 remain = AUDIO_GetDMABytesLeft() / 32;

  /* adjust desired output samplerate if audio playback is not perfectly in sync with video */
  if (remain > 0)
  {
    int samplerate;

    /* check current audio DMA position (from testing, delta keeps limited to +/- one 32-bit block i.e 8 samples) */
    if (remain < 5)
    {
      /* previous frame DMA is not yet finished (real output rate is slightly slower than expected value) */
      samplerate = 47950;
    }
    else
    {
      /* current frame DMA is already started (real output rate is slightly faster than expected value) */
      samplerate = 48050;
    }

    /* update resampler ratios if output samplerate has changed */
    if (samplerate != snd.sample_rate)
    {
      /* this will adjust the number of samples returned on next frame(s) */
      audio_set_rate(samplerate, snd.frame_rate);
    }
  }

  /* clear VSYNC flag */
  videoWait = 0;
}

/* GX draw callback */
static void gx_callback(void)
{
  /* swap framebuffers */
  fbCurrent ^= 1;
  VIDEO_SetNextFramebuffer(xfb[fbCurrent]);
  VIDEO_Flush();

  /* copy EFB to XFB */
  GX_CopyDisp(xfb[fbCurrent], GX_TRUE);
  GX_Flush();
}

/* Initialize GX */
static void gxStart(void)
{
  /*** Clear out FIFO area ***/
  memset(&gp_fifo, 0, sizeof(gp_fifo));

  /*** GX default ***/
  GX_Init(&gp_fifo, sizeof(gp_fifo));
  GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
  GX_SetCullMode(GX_CULL_NONE);
  GX_SetClipMode(GX_CLIP_DISABLE);
  GX_SetDispCopyGamma(GX_GM_1_0);
  GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GX_SetColorUpdate(GX_TRUE);
  GX_SetAlphaUpdate(GX_FALSE);

  /* Modelview */
  Mtx view;
  memset (&view, 0, sizeof (Mtx));
  guLookAt(view, &cam.pos, &cam.up, &cam.view);
  GX_LoadPosMtxImm(view, GX_PNMTX0);
  GX_Flush();
}

/* Reset GX rendering */
static void gxResetRendering(u8 type)
{
  GX_ClearVtxDesc();

  if (type)
  {
    /* uses direct positionning, alpha blending & color channel (menu rendering) */
    GX_SetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxDesc (GX_VA_CLR0, GX_DIRECT);
    /* 
       Color.out = Color.rasterized*Color.texture
       Alpha.out = Alpha.rasterized*Alpha.texture 
    */
    GX_SetTevOp (GX_TEVSTAGE0, GX_MODULATE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetNumTexGens(1);
    GX_SetNumChans(1);

    /* disable GX rendering callback */
    GX_SetDrawDoneCallback(NULL);
  }
  else
  {
    /* uses array positionning, no alpha blending, no color channel (video emulation) */
    GX_SetBlendMode(GX_BM_NONE,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S16, 0);
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX8);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetArray(GX_VA_POS, square, 2 * sizeof (s16));
    /* 
       Color.out = Color.texture
       Alpha.out = Alpha.texture 
    */
    GX_SetTevOp (GX_TEVSTAGE0, GX_REPLACE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetNumTexGens(1);
    GX_SetNumChans(0);

    /* enable GX rendering callback */
    GX_SetDrawDoneCallback(gx_callback);
  }

  GX_Flush();
}

/* Reset GX rendering mode */
static void gxResetMode(GXRModeObj *tvmode, int vfilter_enabled)
{
  Mtx44 p;
  f32 yScale = GX_GetYScaleFactor(tvmode->efbHeight, tvmode->xfbHeight);
  u16 xfbHeight = GX_SetDispCopyYScale(yScale);
  u16 xfbWidth  = VIDEO_PadFramebufferWidth(tvmode->fbWidth);  

  GX_SetCopyClear((GXColor)BLACK,0x00ffffff);
  GX_SetViewport(0.0F, 0.0F, tvmode->fbWidth, tvmode->efbHeight, 0.0F, 1.0F);
  GX_SetScissor(0, 0, tvmode->fbWidth, tvmode->efbHeight);
  GX_SetDispCopySrc(0, 0, tvmode->fbWidth, tvmode->efbHeight);
  GX_SetDispCopyDst(xfbWidth, xfbHeight);
  GX_SetCopyFilter(tvmode->aa, tvmode->sample_pattern, (tvmode->xfbMode == VI_XFBMODE_SF) ? GX_FALSE : vfilter_enabled, tvmode->vfilter);
  GX_SetFieldMode(tvmode->field_rendering, ((tvmode->viHeight == 2 * tvmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
  guOrtho(p, tvmode->efbHeight/2, -(tvmode->efbHeight/2), -(tvmode->fbWidth/2), tvmode->fbWidth/2, 100, 1000);
  GX_LoadProjectionMtx(p, GX_ORTHOGRAPHIC);
  GX_Flush();
}

/* Update Aspect Ratio */
static void gxSetAspectRatio(int *xscale, int *yscale)
{
  /* Vertical Scaling is disabled by default */
  *yscale = (bitmap.viewport.h + (2 * bitmap.viewport.y)) / 2;

  /* Original aspect ratio */
  if (config.aspect)
  {
    /* Adjust vertical scaling when input & output video heights are different */
    if (vdp_pal && (!gc_pal || config.render))
    {
      *yscale = *yscale * VI_MAX_HEIGHT_NTSC / VI_MAX_HEIGHT_PAL;
    }
    else if (!vdp_pal && gc_pal && !config.render)
    {
      *yscale = *yscale * VI_MAX_HEIGHT_PAL / VI_MAX_HEIGHT_NTSC;
    }

    /* Horizontal Scaling */
    /* Wii/Gamecube pixel clock = 13.5 Mhz */
    /* "H32" pixel clock = Master Clock / 10 = 5.3693175 Mhz (NTSC) or 5.3203424 Mhz (PAL) */
    /* "H40" pixel clock = Master Clock / 8 = 6.711646875 Mhz (NTSC) or 6.650428 Mhz (PAL) */
    if (config.overscan & 2)
    {
      /* Horizontal borders are emulated */
      if (reg[12] & 1)
      {
        /* 348 "H40" pixels = 348 * Wii/GC pixel clock / "H40" pixel clock = approx. 700 (NTSC) or 707 (PAL) Wii/GC pixels */
        *xscale = (system_clock == MCLOCK_NTSC) ? 350 : 354; 
      }
      else
      {
        /* 284 "H32" pixels = 284 * Wii/GC pixel clock / "H32" pixel clock = approx. 714 (NTSC) or 721 (PAL) Wii/GC pixels */
        *xscale = (system_clock == MCLOCK_NTSC) ? 357 : 361; 
      }
    }
    else
    {
      /* Horizontal borders are simulated */
      if ((system_hw == SYSTEM_GG) && !config.gg_extra)
      {
        /* 160 "H32" pixels = 160 * Wii/GC pixel clock / "H32" pixel clock = approx. 403 Wii/GC pixels (NTSC only) */
        *xscale = 202;
      }
      else
      {
        /* 320 "H40" pixels = 256 "H32" pixels = 256 * Wii/GC pixel clock / "H32" pixel clock = approx. 644 (NTSC) or 650 (PAL) Wii/GC pixels */
        *xscale = (system_clock == MCLOCK_NTSC) ? 322 : 325; 
      }
    }

    /* Aspect correction for widescreen TV */
    if (config.aspect & 2)
    {
      /* Keep 4:3 aspect ratio on 16:9 output */
      *xscale = (*xscale * 3) / 4;
    }
  }

  /* Manual aspect ratio */
  else
  {
    /* By default, disable horizontal scaling */
    *xscale = bitmap.viewport.w + (2 * bitmap.viewport.x);
      
    /* Keep original aspect ratio in H32 modes */
    if (!(reg[12] & 1))
    {
        *xscale = (*xscale * 320) / 256;
    }

    /* Game Gear specific: if borders are disabled, upscale to fullscreen */
    if ((system_hw == SYSTEM_GG) && !config.gg_extra)
    {
      if (!(config.overscan & 1))
      {
        /* Active area height = approx. 224 non-interlaced lines (60hz) */
        *yscale = 112;
      }

      if (!(config.overscan & 2))
      {
        /* Active area width = approx. 640 pixels */
        *xscale = 320;
      }
    }

    /* By default, keep NTSC aspect ratio */
    if (gc_pal && !config.render)
    {
      /* Upscale PAL output */
      *yscale = *yscale * VI_MAX_HEIGHT_PAL / VI_MAX_HEIGHT_NTSC;
    }

    /* Add user scaling */
    *xscale += config.xscale;
    *yscale += config.yscale;
  }
}

/* Reset GX/VI hardware scaler */
static void gxResetScaler(u32 width)
{
  int xscale  = 0;
  int yscale  = 0;
  int offset  = 0;

  /* retrieve screen aspect ratio */
  gxSetAspectRatio(&xscale, &yscale);

  /* default EFB width */
  rmode->fbWidth = 640;

  /* no filtering, disable GX horizontal scaling */
  if (!config.bilinear && !config.ntsc)
  {
    if ((width <= 320) && (width <= xscale))
      rmode->fbWidth = width * 2;
    else if (width <= 640)
      rmode->fbWidth = width;
  }

  /* configure VI width */
  if ((xscale * 2) > rmode->fbWidth)
  {
    /* max width = 720 pixels */
    if (xscale > 360)
    {
      /* save offset for later */
      offset = ((xscale - 360) * rmode->fbWidth) / rmode->viWidth;

      /* maximal width */
      xscale = 360;
    }

    /* enable VI upscaling */
    rmode->viWidth = xscale * 2;
    rmode->viXOrigin = (720 - (xscale * 2)) / 2;

    /* default GX horizontal scaling */
    xscale = (rmode->fbWidth / 2);

    /* handle additional upscaling */
    if (offset)
    {
      /* no filtering, reduce EFB width to increase VI upscaling */
      if (!config.bilinear && !config.ntsc)
        rmode->fbWidth -= (offset * 2);
      
      /* increase GX horizontal scaling */
      else
        xscale += offset;
    }
  }
  else
  {
    /* VI horizontal scaling is disabled */
    rmode->viWidth = rmode->fbWidth;
    rmode->viXOrigin = (720 - rmode->fbWidth) / 2;
  }

  /* Adjust screen position */
  int xshift = (config.xshift * rmode->fbWidth) / rmode->viWidth;
  int yshift = (config.yshift * rmode->efbHeight) / rmode->viHeight;

  /* Double Resolution modes (480i/576i/480p) */
  if (config.render)
  {
    yscale = yscale * 2;
  }

  /* Set GX scaler (Vertex Position matrix) */
  square[0] = square[6] = xshift - xscale;
  square[2] = square[4] = xshift + xscale;
  square[1] = square[3] = yshift + yscale;
  square[5] = square[7] = yshift - yscale;
  DCStoreRange(square, 32);
  GX_InvVtxCache();
}

static void gxDrawCrosshair(gx_texture *texture, int x, int y)
{
  /* adjust texture dimensions to XFB->VI scaling */
  int w = (texture->width * rmode->fbWidth) / (rmode->viWidth);
  int h = (texture->height * rmode->efbHeight) / (rmode->viHeight);

  /* Aspect correction for widescreen TV */
  if (config.aspect & 2) w = (w * 3) / 4;

  /* EFB scale & shift */
  int xwidth = square[2] - square[6];
  int ywidth = square[3] - square[7];

  /* adjust texture coordinates to EFB */
  x = (((x + bitmap.viewport.x) * xwidth) / (bitmap.viewport.w+2*bitmap.viewport.x)) + square[6] - w/2;
  y = (((y + bitmap.viewport.y) * ywidth) / (bitmap.viewport.h+2*bitmap.viewport.y)) + square[7] - h/2;

  /* load texture object */
  GX_LoadTexObj(&texture->texObj, GX_TEXMAP0);
  GX_InvalidateTexAll();

  /* Draw textured quad */
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position2s16(x,y+h);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(0.0, 1.0);
  GX_Position2s16(x+w,y+h);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(1.0, 1.0);
  GX_Position2s16(x+w,y);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(1.0, 0.0);
  GX_Position2s16(x,y);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(0.0, 0.0);
  GX_End();
}

static void gxDrawCdLeds(gx_texture *texture_l, gx_texture *texture_r)
{
  /* adjust texture dimensions to XFB->VI scaling */
  int w = (texture_l->width * rmode->fbWidth) / (rmode->viWidth);
  int h = (texture_l->height * rmode->efbHeight) / (rmode->viHeight);

  /* Aspect correction for widescreen TV */
  if (config.aspect & 2) w = (w * 3) / 4;

  /* EFB scale & shift */
  int xwidth = square[2] - square[6];
  int ywidth = square[3] - square[7];

  /* adjust texture coordinates to EFB */
  int xl = ((bitmap.viewport.x * xwidth) / vwidth) + square[6] + 8;
  int xr = (((bitmap.viewport.x + bitmap.viewport.w) * xwidth) / vwidth) + square[6] - 8 - w;
  int y = (((bitmap.viewport.y + bitmap.viewport.h - 4) * ywidth) / vheight) + square[7] - h;

  /* load left screen texture */
  GX_LoadTexObj(&texture_l->texObj, GX_TEXMAP0);
  GX_InvalidateTexAll();

  /* Draw textured quad */
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position2s16(xl,y+h);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(0.0, 1.0);
  GX_Position2s16(xl+w,y+h);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(1.0, 1.0);
  GX_Position2s16(xl+w,y);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(1.0, 0.0);
  GX_Position2s16(xl,y);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(0.0, 0.0);
  GX_End();

  /* load right screen texture */
  GX_LoadTexObj(&texture_r->texObj, GX_TEXMAP0);
  GX_InvalidateTexAll();

  /* Draw textured quad */
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position2s16(xr,y+h);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(0.0, 1.0);
  GX_Position2s16(xr+w,y+h);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(1.0, 1.0);
  GX_Position2s16(xr+w,y);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(1.0, 0.0);
  GX_Position2s16(xr,y);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(0.0, 0.0);
  GX_End();
}

static void gxDrawOnScreenText(char *msg)
{
  GXRModeObj *temp = vmode;
  int y = (40 * rmode->efbHeight) / 480;
  int x = (bitmap.viewport.x > 0) ? (24 + bitmap.viewport.x) : 24;
  x = (x * rmode->fbWidth) / rmode->viWidth;

  vmode = rmode;
  FONT_write(msg, 20, x, y, rmode->fbWidth, (GXColor)WHITE);
  vmode = temp;
}

void gxDrawRectangle(s32 x, s32 y, s32 w, s32 h, u8 alpha, GXColor color)
{
  /* GX only use Color channel for rendering */
  GX_SetTevOp (GX_TEVSTAGE0, GX_PASSCLR);
  GX_SetVtxDesc (GX_VA_TEX0, GX_NONE);
  GX_Flush();

  /* vertex coordinate */
  x -= (vmode->fbWidth/2);
  y -= (vmode->efbHeight/2);

  /* draw colored quad */
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position2s16(x,y+h);
  GX_Color4u8(color.r,color.g,color.b,alpha);
  GX_Position2s16(x+w,y+h);
  GX_Color4u8(color.r,color.g,color.b,alpha);
  GX_Position2s16(x+w,y);
  GX_Color4u8(color.r,color.g,color.b,alpha);
  GX_Position2s16(x,y);
  GX_Color4u8(color.r,color.g,color.b,alpha);
  GX_End();
  GX_DrawDone();

  /* restore GX rendering */
  GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GX_SetTevOp (GX_TEVSTAGE0, GX_MODULATE);
  GX_Flush();
}

void gxDrawTexture(gx_texture *texture, s32 x, s32 y, s32 w, s32 h, u8 alpha)
{
  if (!texture) return;
  if (texture->data)
  {
    /* load texture object */
    GX_LoadTexObj(&texture->texObj, GX_TEXMAP0);
    GX_InvalidateTexAll();

    /* vertex coordinate */
    x -= (vmode->fbWidth/2);
    y -= (vmode->efbHeight/2);

    /* draw textured quad */
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2s16(x,y+h);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(0.0, 1.0);
    GX_Position2s16(x+w,y+h);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(1.0, 1.0);
    GX_Position2s16(x+w,y);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(1.0, 0.0);
    GX_Position2s16(x,y);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(0.0, 0.0);
    GX_End();
    GX_DrawDone();
  }
}

void gxDrawTextureRotate(gx_texture *texture, s32 x, s32 y, s32 w, s32 h, f32 angle, u8 alpha)
{
  if (!texture) return;
  if (texture->data)
  {
    /* load texture object */
    GX_LoadTexObj(&texture->texObj, GX_TEXMAP0);
    GX_InvalidateTexAll();

    /* vertex coordinate */
    x -= (vmode->fbWidth/2);
    y -= (vmode->efbHeight/2);

    /* Modelview rotation */
    Mtx m,mv;
    guVector axis = (guVector) {0,0,1};
    guLookAt(mv, &cam.pos, &cam.up, &cam.view);
    guMtxRotAxisDeg (m, &axis, angle);
    guMtxTransApply(m,m, x+w/2,y+h/2,0);
    guMtxConcat(mv,m,mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_Flush();

    /* draw textured quad */
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2s16(-w/2,-h/2);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(0.0, 0.0);
    GX_Position2s16(w/2,-h/2);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(1.0, 0.0);
    GX_Position2s16(w/2,h/2);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(1.0, 1.0);
    GX_Position2s16(-w/2,h/2);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(0.0, 1.0);
    GX_End();
    GX_DrawDone();

    /* restore default Modelview */
    guLookAt(mv, &cam.pos, &cam.up, &cam.view);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_Flush();
  }
}

void gxDrawTextureRepeat(gx_texture *texture, s32 x, s32 y, s32 w, s32 h, u8 alpha)
{
  if (!texture) return;
  if (texture->data)
  {
    /* load texture object */
    GXTexObj texObj;
    GX_InitTexObj(&texObj, texture->data, texture->width, texture->height, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_LoadTexObj(&texObj, GX_TEXMAP0);
    GX_InvalidateTexAll();

    /* vertex coordinate */
    x -= (vmode->fbWidth/2);
    y -= (vmode->efbHeight/2);

    /* texture coordinates */
    f32 s = (f32)w / (f32)texture->width;
    f32 t = (f32)h / (f32)texture->height;

    /* draw textured quad */
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2s16(x,y+h);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(0.0, t);
    GX_Position2s16(x+w,y+h);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(s, t);
    GX_Position2s16(x+w,y);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(s, 0.0);
    GX_Position2s16(x,y);
    GX_Color4u8(0xff,0xff,0xff,alpha);
    GX_TexCoord2f32(0.0, 0.0);
    GX_End();
    GX_DrawDone();
  }
}

void gxDrawScreenshot(u8 alpha)
{
  if (!rmode) return;

  /* get current game screen texture */
  GX_LoadTexObj(&screenTexObj, GX_TEXMAP0);
  GX_InvalidateTexAll();

  /* get current aspect ratio */
  int xscale,yscale;
  gxSetAspectRatio(&xscale, &yscale);

  /* adjust horizontal scaling */
  xscale = (xscale * vmode->fbWidth) / vmode->viWidth;

  /* adjust screen position */
  int xshift = (config.xshift * vmode->fbWidth) / vmode->viWidth;
  int yshift = (config.yshift * vmode->efbHeight) / vmode->viHeight;

  /* set vertices position & size */
  s32 x = xshift - xscale;
  s32 y = yshift - (yscale * 2);
  s32 w = xscale * 2;
  s32 h = yscale * 4;

  /* black out surrounding area if necessary (Game Gear without borders) */
  if ((w < 640) || (h < 480))
  {
    gxDrawRectangle(0, 0, 640, 480, 255, (GXColor)BLACK);
  }

  /* draw textured quad */
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position2s16(x,y+h);
  GX_Color4u8(0xff,0xff,0xff,alpha);
  GX_TexCoord2f32(0.0, 1.0);
  GX_Position2s16(x+w,y+h);
  GX_Color4u8(0xff,0xff,0xff,alpha);
  GX_TexCoord2f32(1.0, 1.0);
  GX_Position2s16(x+w,y);
  GX_Color4u8(0xff,0xff,0xff,alpha);
  GX_TexCoord2f32(1.0, 0.0);
  GX_Position2s16(x,y);
  GX_Color4u8(0xff,0xff,0xff,alpha);
  GX_TexCoord2f32(0.0, 0.0);
  GX_End();
  GX_DrawDone();
}

void gxCopyScreenshot(gx_texture *texture)
{
  /* retrieve gamescreen texture */
  GX_LoadTexObj(&screenTexObj, GX_TEXMAP0);
  GX_InvalidateTexAll();

  /* scale texture to EFB width */
  s32 w = ((bitmap.viewport.w + 2*bitmap.viewport.x) * 640) / bitmap.viewport.w;
  s32 h = (bitmap.viewport.h + 2*bitmap.viewport.y) * 2;
  s32 x = -w/2;
  s32 y = -(240+ 2*bitmap.viewport.y);

  /* black out surrounding area if necessary (Game Gear without borders) */
  if ((w < 640) || (h < 480))
  {
    gxDrawRectangle(0, 0, 640, 480, 255, (GXColor)BLACK);
  }

  /* draw textured quad */
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position2s16(x,y+h);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(0.0, 1.0);
  GX_Position2s16(x+w,y+h);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(1.0, 1.0);
  GX_Position2s16(x+w,y);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(1.0, 0.0);
  GX_Position2s16(x,y);
  GX_Color4u8(0xff,0xff,0xff,0xff);
  GX_TexCoord2f32(0.0, 0.0);
  GX_End();
  GX_DrawDone();

  /* copy EFB to texture */
  GX_SetTexCopySrc(0, 0, texture->width * 2, texture->height * 2);
  GX_SetTexCopyDst(texture->width, texture->height, texture->format, GX_TRUE);
  GX_CopyTex(texture->data, GX_TRUE);
  GX_Flush();

  /* wait for copy operation to finish */
  GX_PixModeSync();

  /* invalidate data cache area corresponding to texture RAM address */
  DCInvalidateRange(texture->data, texture->width * texture->height * 4);
}

/* Take Screenshot */
void gxSaveScreenshot(char *filename)
{
  /* capture screenshot into a texture */
  gx_texture texture;
  texture.format = GX_TF_RGBA8;
  texture.width = 320;
  texture.height = bitmap.viewport.h;
  texture.data = memalign(32, texture.width*texture.height*4);

  if (texture.data)
  {
    gxCopyScreenshot(&texture);

    /* open PNG file */
    FILE *f = fopen(filename,"wb");
    if (f)
    {
      /* encode screenshot into PNG file */
      gxTextureWritePNG(&texture,f);
      fclose(f);
    }

    free(texture.data);
  }
}

void gxSetScreen(void)
{
  VIDEO_WaitVSync();
  GX_CopyDisp(xfb[fbCurrent], GX_FALSE);
  GX_Flush();
  gx_input_UpdateMenu();
}

void gxClearScreen(GXColor color)
{
  gxDrawRectangle(0, 0, vmode->fbWidth, vmode->efbHeight, 255, color);
}

/***************************************************************************************/
/*   GX Texture <-> LibPNG routines                                                    */
/***************************************************************************************/

/* libpng read callback function */
static void png_read_from_mem (png_structp png_ptr, png_bytep data, png_size_t length)
{
  png_image_desc *image = (png_image_desc *)png_get_io_ptr(png_ptr);

  /* copy data from image buffer */
  memcpy (data, image->buffer + image->offset, length);

  /* advance in the file */
  image->offset += length;
}

/* convert PNG image (from file or data buffer) into RGBA8 texture */
gx_texture *gxTextureOpenPNG(const u8 *png_data, FILE *png_file)
{
  int i;

  /* create a png read struct */
  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
  if (!png_ptr) return NULL;

  /* create a png info struct */
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    png_destroy_read_struct(&png_ptr,NULL,NULL);
    return NULL;
  }

  if (png_data)
  {
    /* init PNG image structure */
    png_image_desc image;
    image.buffer = (u8 *) png_data;
    image.offset = 0;

    /* set callback for the read function */
    png_set_read_fn(png_ptr,(png_voidp *)(&image),png_read_from_mem);
  }
  else if (png_file)
  {
    /* check for valid magic number */
    png_byte magic[8];
    if (fread (magic, 1, 8, png_file) != 8)
    {
      png_destroy_read_struct(&png_ptr,&info_ptr,NULL);
      return NULL;
    }

    if (png_sig_cmp (magic, 0, 8))
    {
      png_destroy_read_struct(&png_ptr,&info_ptr,NULL);
      return NULL;
    }

    /* set IO callback for read function */
    png_init_io (png_ptr, png_file);
    png_set_sig_bytes (png_ptr, 8);
  }
  else
  {
    png_destroy_read_struct(&png_ptr,&info_ptr,NULL);
    return NULL;
  }

  /* read png info */
  png_read_info(png_ptr,info_ptr);

  /* retrieve image information */
  u32 width  = png_get_image_width(png_ptr,info_ptr);
  u32 height = png_get_image_height(png_ptr,info_ptr);
  u32 bit_depth = png_get_bit_depth(png_ptr,info_ptr);
  u32 color_type = png_get_color_type(png_ptr,info_ptr);

  /* ensure PNG file is in the supported format */
  if (png_file)
  {
    /* support for RGBA8 textures ONLY !*/
    if ((color_type != PNG_COLOR_TYPE_RGB_ALPHA) || (bit_depth != 8))
    {
      png_destroy_read_struct(&png_ptr, &info_ptr,NULL);
      return NULL;
    }

    /* 4x4 tiles are required */
    if ((width%4) || (height%4))
    {
      png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
      return NULL;
    }
  }

  /* allocate memory to store raw image data */
  u32 stride = width << 2;
  u8 *img_data = memalign (32, stride * height);
  if (!img_data)
  {
    png_destroy_read_struct(&png_ptr,&info_ptr,NULL);
    return NULL;
  }

  /* allocate row pointer data */
  png_bytep *row_pointers = (png_bytep *)memalign (32, sizeof (png_bytep) * height);
  if (!row_pointers)
  {
    free (img_data);
    png_destroy_read_struct(&png_ptr,&info_ptr,NULL);
    return NULL;
  }

  /* store raw image data */
  for (i = 0; i < height; i++)
  {
    row_pointers[i] = img_data + (i * stride);
  }

  /* decode image */
  png_read_image(png_ptr, row_pointers);

  /* finish decompression and release memory */
  png_read_end(png_ptr, NULL);
  png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
  free(row_pointers);

  /* allocate texture */
  gx_texture *texture = (gx_texture *)memalign(32, sizeof(gx_texture));
  if (!texture)
  {
    free (img_data);
    return NULL;
  }

  /* allocate texture data */
  texture->data = memalign(32, stride * height);
  if (!texture->data)
  {
    free (img_data);
    free(texture);
    return NULL;
  }

  /* initialize texture */
  memset(texture->data, 0, stride * height);
  texture->width  = width;
  texture->height = height;
  texture->format = GX_TF_RGBA8;

  /* initialize GX texture object */
  GX_InitTexObj(&texture->texObj, texture->data, width, height, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);

  /* encode to GX_TF_RGBA8 format (4x4 pixels paired titles) */
  u16 *dst_ar = (u16 *)(texture->data);
  u16 *dst_gb = (u16 *)(texture->data + 32);
  u32 *src1 = (u32 *)(img_data);
  u32 *src2 = (u32 *)(img_data + stride);
  u32 *src3 = (u32 *)(img_data + 2*stride);
  u32 *src4 = (u32 *)(img_data + 3*stride);
  u32 pixel,h,w;

  for (h=0; h<height; h+=4)
  {
    for (w=0; w<width; w+=4)
    {
      /* line N (4 pixels) */
      for (i=0; i<4; i++)
      {
        pixel = *src1++;
        *dst_ar++= ((pixel << 8) & 0xff00) | ((pixel >> 24) & 0x00ff);
        *dst_gb++= (pixel >> 8) & 0xffff;
      }

      /* line N + 1 (4 pixels) */
      for (i=0; i<4; i++)
      {
        pixel = *src2++;
        *dst_ar++= ((pixel << 8) & 0xff00) | ((pixel >> 24) & 0x00ff);
        *dst_gb++= (pixel >> 8) & 0xffff;
      }

      /* line N + 2 (4 pixels) */
      for (i=0; i<4; i++)
      {
        pixel = *src3++;
        *dst_ar++= ((pixel << 8) & 0xff00) | ((pixel >> 24) & 0x00ff);
        *dst_gb++= (pixel >> 8) & 0xffff;
      }

      /* line N + 3 (4 pixels) */
      for (i=0; i<4; i++)
      {
        pixel = *src4++;
        *dst_ar++= ((pixel << 8) & 0xff00) | ((pixel >> 24) & 0x00ff);
        *dst_gb++= (pixel >> 8) & 0xffff;
      }

      /* next paired tiles */
      dst_ar += 16;
      dst_gb += 16;
    }

    /* next 4 lines */
    src1 = src4;
    src2 = src1 + width;
    src3 = src2 + width;
    src4 = src3 + width;
  }

  /* release memory */
  free(img_data);

  /* force texture data update in memory */
  DCStoreRange(texture->data, height * stride);

  return texture;
}

/* Write RGBA8 Texture to PNG file */
void gxTextureWritePNG(gx_texture *texture, FILE *png_file)
{
  /* allocate PNG data buffer */
  u8 *img_data = (u8 *)memalign(32, texture->width * texture->height * 4);
  if(!img_data) return;

  /* decode GX_TF_RGBA8 format (4x4 pixels paired titles) */
  u16 *ar = (u16 *)(texture->data);
  u16 *gb = (u16 *)(texture->data + 32);
  u32 *dst1 = (u32 *)(img_data);
  u32 *dst2 = dst1 + texture->width;
  u32 *dst3 = dst2 + texture->width;
  u32 *dst4 = dst3 + texture->width;
  u32 i,h,w,pixel;

  for (h=0; h<texture->height; h+=4)
  {
    for (w=0; w<texture->width; w+=4)
    {
      /* line N (4 pixels) */
      for (i=0; i<4; i++)
      {
        pixel = ((*ar & 0xff) << 24) | (*gb << 8) | ((*ar & 0xff00) >> 8);
        *dst1++ = pixel;
        ar++;
        gb++;
      }

      /* line N + 1 (4 pixels) */
      for (i=0; i<4; i++)
      {
        pixel = ((*ar & 0xff) << 24) | (*gb << 8) | ((*ar & 0xff00) >> 8);
        *dst2++ = pixel;
        ar++;
        gb++;
      }

      /* line N + 2 (4 pixels) */
      for (i=0; i<4; i++)
      {
        pixel = ((*ar & 0xff) << 24) | (*gb << 8) | ((*ar & 0xff00) >> 8);
        *dst3++ = pixel;
        ar++;
        gb++;
      }

      /* line N + 3 (4 pixels) */
      for (i=0; i<4; i++)
      {
        pixel = ((*ar & 0xff) << 24) | (*gb << 8) | ((*ar & 0xff00) >> 8);
        *dst4++ = pixel;
        ar++;
        gb++;
      }

      /* next paired tiles */
      ar += 16;
      gb += 16;
    }

    /* next 4 lines */
    dst1 = dst4;
    dst2 = dst1 + texture->width;
    dst3 = dst2 + texture->width;
    dst4 = dst3 + texture->width;
  }

  /* create a png write struct */
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png_ptr)
  {
    free(img_data);
    return;
  }

  /* create a png info struct */
  png_infop info_ptr = png_create_info_struct (png_ptr);
  if (!info_ptr)
  {
    free(img_data);
    png_destroy_write_struct(&png_ptr, NULL);
    return;
  }

  /* set IO callback for the write function */
  png_init_io(png_ptr, png_file);

  /* set PNG file properties */
  png_set_IHDR(png_ptr, info_ptr, texture->width, texture->height, 8, PNG_COLOR_TYPE_RGB_ALPHA, 
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  /* allocate row pointer data */
  png_bytep *row_pointers = (png_bytep *)memalign (32, sizeof (png_bytep) * texture->height);
  if (!row_pointers)
  {
    free (img_data);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return;
  }

  /* store raw image data */
  for (i = 0; i < texture->height; i++)
  {
    row_pointers[i] = img_data + (i * texture->width * 4);
  }

  /* configure libpng for image data */
  png_set_rows(png_ptr,info_ptr,row_pointers);

  /* write data to PNG file */
  png_write_png(png_ptr,info_ptr,PNG_TRANSFORM_IDENTITY,NULL);

  /* finish compression and release memory */
  png_write_end(png_ptr, NULL);
  free(row_pointers);
  free(img_data);
  png_destroy_write_struct(&png_ptr, &info_ptr);
}


void gxTextureClose(gx_texture **p_texture)
{
  gx_texture *texture = *p_texture;

  if (texture)
  {
    if (texture->data) free(texture->data);
    free(texture);
    *p_texture = NULL;
  }
}


/***************************************************************************************/
/*   VIDEO engine                                                                      */
/***************************************************************************************/

/* Emulation mode -> Menu mode */
void gx_video_Stop(void)
{
  /* disable VSYNC callbacks */
  VIDEO_SetPostRetraceCallback(NULL);
  
  /* wait for next even field */
  /* this prevents screen artefacts when switching between interlaced & non-interlaced modes */
  do VIDEO_WaitVSync();
  while (!VIDEO_GetNextField());
   
  /* adjust TV width */
  vmode->viWidth = config.screen_w;
  vmode->viXOrigin = (VI_MAX_WIDTH_NTSC - vmode->viWidth)/2;

  /* unallocate NTSC filters */
  if (sms_ntsc) free(sms_ntsc);
  if (md_ntsc) free(md_ntsc);
  sms_ntsc = NULL;
  md_ntsc = NULL;

  /* lightgun textures */
  gxTextureClose(&crosshair[0]);
  gxTextureClose(&crosshair[1]);

  /* CD leds textures */
  gxTextureClose(&cd_leds[0][0]);
  gxTextureClose(&cd_leds[0][1]);
  gxTextureClose(&cd_leds[1][0]);
  gxTextureClose(&cd_leds[1][1]);

  /* GX menu rendering */
  gxResetRendering(1);
  gxResetMode(vmode, GX_TRUE);

  /* render game snapshot */
  gxClearScreen((GXColor)BLACK);
  gxDrawScreenshot(0xff);

#ifdef HW_RVL
  /* default VI settings */
  VIDEO_SetTrapFilter(1);
  VIDEO_SetGamma(VI_GM_1_0);
#endif

  /* reset default TV mode */
  VIDEO_Configure(vmode);
  VIDEO_Flush();

  /* display next frame */
  gxSetScreen();
}

/* Menu mode -> Emulation mode */
void gx_video_Start(void)
{
  /* TV mode */
  if ((config.tv_mode == 1) || ((config.tv_mode == 2) && vdp_pal))
  {
    /* 50 Hz */
    gc_pal = 1;
  }
  else
  {
    /* 60 Hz */
    gc_pal = 0;
  }

  /* Enable progressive or interlaced video mode */
  if (config.render == 2)
  {
    /* 480p */
    tvmodes[2]->viTVMode = (tvmodes[2]->viTVMode & ~3) | VI_PROGRESSIVE;
    tvmodes[2]->xfbMode = VI_XFBMODE_SF;

    /* 576p */
    tvmodes[5]->viTVMode = VI_TVMODE_PAL_PROG;
    tvmodes[5]->xfbMode = VI_XFBMODE_SF;
  }
  else if (config.render == 1)
  {
    /* 480i */
    tvmodes[2]->viTVMode = (tvmodes[2]->viTVMode & ~3) | VI_INTERLACE;
    tvmodes[2]->xfbMode = VI_XFBMODE_DF;

    /* 576i */
    tvmodes[5]->viTVMode = VI_TVMODE_PAL_INT;
    tvmodes[5]->xfbMode = VI_XFBMODE_DF;
  }

  /* update horizontal border width */
  if ((system_hw == SYSTEM_GG) && !config.gg_extra)
  {
    bitmap.viewport.x = (config.overscan & 2) ? 14 : -48;
  }
  else
  {
    bitmap.viewport.x = (config.overscan & 2) * 7;
  }

  /* force viewport update */
  bitmap.viewport.changed = 3;

  /* NTSC filter */
  if (config.ntsc)
  {
    /* allocate filters */
    if (!sms_ntsc)
    {
      sms_ntsc = (sms_ntsc_t *)memalign(32,sizeof(sms_ntsc_t));
    }
    if (!md_ntsc)
    {
      md_ntsc = (md_ntsc_t *)memalign(32,sizeof(md_ntsc_t));
    }

    /* setup filters default configuration */
    switch (config.ntsc)
    {
      case 1:
      {
        sms_ntsc_init(sms_ntsc, &sms_ntsc_composite);
        md_ntsc_init(md_ntsc, &md_ntsc_composite);
        break;
      }

      case 2:
      {
        sms_ntsc_init(sms_ntsc, &sms_ntsc_svideo);
        md_ntsc_init(md_ntsc, &md_ntsc_svideo);
        break;
      }

      case 3:
      {
        sms_ntsc_init(sms_ntsc, &sms_ntsc_rgb);
        md_ntsc_init(md_ntsc, &md_ntsc_rgb);
      }

      case 4:
      {
        md_ntsc_setup_t ntsc_custom = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        ntsc_custom.sharpness  = config.ntsc_sharpness;
        ntsc_custom.resolution = config.ntsc_resolution;
        ntsc_custom.artifacts  = config.ntsc_artifacts;
        ntsc_custom.fringing   = config.ntsc_fringing;
        ntsc_custom.bleed      = config.ntsc_bleed;
        sms_ntsc_init(sms_ntsc, (sms_ntsc_setup_t *)&ntsc_custom);
        md_ntsc_init(md_ntsc, &ntsc_custom);
        break;
      }
    }
  }

  /* on-screen display enable flag */
  osd = config.fps;

  /* clear any on-screen text */
  memset(msg, 0, sizeof(msg));

  /* lightgun textures */
  int i, player = 0;
  for (i=0; i<MAX_DEVICES; i++)
  {
    /* Check for emulated lightguns */
    if (input.dev[i] == DEVICE_LIGHTGUN)
    {
      /* Check if input device is affected to player */
      if (config.input[player].device >= 0)
      {
        if ((i == 0) || ((i == 4) && (input.system[1] != SYSTEM_LIGHTPHASER)))
        {
          /* Lightgun #1 */
          if (config.gun_cursor[0])
          {
            crosshair[0] = gxTextureOpenPNG(Crosshair_p1_png,0);
            osd = 1;
          }
        }
        else
        {
          /* Lightgun #2 */
          if (config.gun_cursor[1])
          {
            crosshair[1] = gxTextureOpenPNG(Crosshair_p2_png,0);
            osd = 1;
          }
        }
      }
    }

    /* Check for any emulated device */
    if (input.dev[i] != NO_DEVICE)
    {
      /* increment player index */
      player++;
    }
  }

  /* CD leds textures */
  if (system_hw == SYSTEM_MCD)
  {
    if (config.cd_leds)
    {
      cd_leds[0][0] = gxTextureOpenPNG(CD_access_off_png,0);
      cd_leds[0][1] = gxTextureOpenPNG(CD_access_on_png,0);
      cd_leds[1][0] = gxTextureOpenPNG(CD_ready_off_png,0);
      cd_leds[1][1] = gxTextureOpenPNG(CD_ready_on_png,0);
      osd = 1;
    }
  }

  /* GX emulation rendering */
  gxClearScreen((GXColor)BLACK);
  gxResetRendering(0);

  /* resynchronize emulation with VSYNC */
  do VIDEO_WaitVSync();
  while (!VIDEO_GetNextField());

#ifdef HW_RVL
  VIDEO_SetTrapFilter(config.trap);
  VIDEO_SetGamma((int)(config.gamma * 10.0));
#endif

  /* Emulation is synchronized with video hardware if VSYNC is set to AUTO & TV mode matches emulated video mode */
  if (config.vsync && (gc_pal == vdp_pal))
  {
    VIDEO_SetPostRetraceCallback(vi_callback);
    videoSync =  VIDEO_WAIT;
  }
  else
  {
    videoSync = 0;
  }

  /* restart frame sync */
  videoWait  = 0;
  frameCount = 0;
  starttime  = gettime();
}

/* GX render update */
int gx_video_Update(int status)
{
  /* make sure current video frame has not already been updated */
  if (status & VIDEO_UPDATE)
  {
    /* set video wait flag if VSYNC is enabled */
    videoWait = videoSync;

    /* check if display has changed during frame */
    if (bitmap.viewport.changed & 1)
    {
      /* update texture size */
      vwidth = bitmap.viewport.w + (2 * bitmap.viewport.x);
      vheight = bitmap.viewport.h + (2 * bitmap.viewport.y);

      /* interlaced mode */
      if (config.render && interlaced)
      {
        vheight = vheight << 1;
      }

      /* ntsc filter */
      if (config.ntsc)
      {
        vwidth = (reg[12] & 1) ? MD_NTSC_OUT_WIDTH(vwidth) : SMS_NTSC_OUT_WIDTH(vwidth);

        /* texel width must remain multiple of 4 */
        vwidth  = (vwidth >> 2) << 2;
      }

      /* initialize texture object */
      GX_InitTexObj(&screenTexObj, bitmap.data, vwidth, vheight, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);

      /* configure texture filtering */
      if (!config.bilinear)
      {
        GX_InitTexObjFilterMode(&screenTexObj, GX_NEAR, GX_NEAR);
      }

      /* load texture object */
      GX_LoadTexObj(&screenTexObj, GX_TEXMAP0);

      /* update rendering mode */
      if (config.render)
      {
        rmode = tvmodes[gc_pal*3 + 2];
      }
      else
      {
        rmode = tvmodes[gc_pal*3 + interlaced];
      }

      /* update aspect ratio */
      gxResetScaler(vwidth);

      /* update GX rendering mode */
      gxResetMode(rmode, config.vfilter);

      /* update VI mode */
      VIDEO_Configure(rmode);
      VIDEO_Flush();
    }

    /* texture is now directly mapped by the line renderer */

    /* force texture cache update */
    DCStoreRange(bitmap.data, vwidth*vheight*2);
    GX_InvalidateTexAll();

    /* render textured quad */
    GX_CallDispList(screenDisplayList, 32);

    /* on-screen display */
    if (osd)
    {
      /* reset GX rendering mode */
      gxResetRendering(1);

      /* lightgun # 1 screen mark */
      if (crosshair[0])
      {
        if (input.system[0] == SYSTEM_LIGHTPHASER)
        {
          gxDrawCrosshair(crosshair[0], input.analog[0][0],input.analog[0][1]);
        }
        else
        {
          gxDrawCrosshair(crosshair[0], input.analog[4][0],input.analog[4][1]);
        }
      }

      /* lightgun #2 screen mark */
      if (crosshair[1])
      {
        if (input.system[1] == SYSTEM_LIGHTPHASER)
        {
          gxDrawCrosshair(crosshair[1], input.analog[4][0],input.analog[4][1]);
        }
        else
        {
          gxDrawCrosshair(crosshair[1], input.analog[5][0],input.analog[5][1]);
        }
      }

      /* CD LEDS */
      if (cd_leds[1][1])
      {
        /* CD LEDS status */
        u8 mode = scd.regs[0x06 >> 1].byte.h;
        gxDrawCdLeds(cd_leds[1][(mode >> 1) & 1], cd_leds[0][mode & 1]);
      }

      /* FPS counter */
      if (config.fps)
      {
        u32 delta = diff_usec(starttime, gettime());
        frameCount++;
        if (delta > 1000000)
        {
          sprintf(msg,"%3.2f FPS", (float)frameCount * 1000000.0 / (float)delta);
          frameCount = 0;
          starttime = gettime();
        }

        /* disable EFB alpha blending for text background */
        GX_SetBlendMode(GX_BM_NONE,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR);
        GX_Flush();

        /* display on-screen message */
        gxDrawOnScreenText(msg);
      }

      /* restore texture object */
      GX_LoadTexObj(&screenTexObj, GX_TEXMAP0);

      /* restore GX rendering mode */
      gxResetRendering(0);
    }

    /* Do not wait for EFB rendering, GX draw interrupt will be triggered when it is finished */
    GX_SetDrawDone();

    /* check interlaced mode change */
    if (bitmap.viewport.changed & 4)
    {
      /* "original" mode */
      if (!config.render && config.vsync && (gc_pal == vdp_pal))
      {
        /* framerate has changed, reinitialize audio timings */
        audio_set_rate(snd.sample_rate, get_framerate());
      }

      /* clear flag */
      bitmap.viewport.changed &= ~4;
    }

    /* Check if video mode has changed */
    if (bitmap.viewport.changed & 1)
    {
      /* clear viewport update flags */
      bitmap.viewport.changed &= ~1;

      /* field synchronization */
      do VIDEO_WaitVSync();
      while (VIDEO_GetNextField() != odd_frame);

      /* resynchronize audio playback with video */                    
      AUDIO_StopDMA();
      AUDIO_StartDMA();
    }
  }

  /* wait until current video frame starts before emulating next frame */
  return ((status & ~(VIDEO_WAIT|VIDEO_UPDATE)) | videoWait);
}

/* Initialize VIDEO subsystem */
void gx_video_Init(void)
{
  /*
   * Before doing anything else under libogc,
   * Call VIDEO_Init
   */
  VIDEO_Init();

  /* Get the current VIDEO mode then :
      - set menu video mode (480p/576p/480i/576i)
      - set emulator rendering 60hz TV modes (MPAL/NTSC/EURGB60)
   */
  vmode = VIDEO_GetPreferredMode(NULL);

  /* Adjust display settings */
  switch (vmode->viTVMode >> 2)
  {
    case VI_PAL:  /* 576 lines scaled (PAL 50Hz) */
      TV60hz_240p.viTVMode = VI_TVMODE_EURGB60_DS;
      TV60hz_240i.viTVMode = VI_TVMODE_EURGB60_INT;
      TV60hz_480i.viTVMode = VI_TVMODE_EURGB60_INT;
      break;
    
    default:  /* 480 lines (NTSC, MPAL or PAL 60Hz) */
      TV60hz_240p.viTVMode = VI_TVMODE(vmode->viTVMode >> 2, VI_NON_INTERLACE);
      TV60hz_240i.viTVMode = VI_TVMODE(vmode->viTVMode >> 2, VI_INTERLACE);
      TV60hz_480i.viTVMode = VI_TVMODE(vmode->viTVMode >> 2, VI_INTERLACE);
      break;
  }

  /* Configure VI */
  VIDEO_Configure(vmode);

  /* Initialize font first (to ensure IPL font buffer is allocated in MEM1 as DMA from EXI bus to MEM2 is apparently not possible with libogc) */
  FONT_Init();

  /* Allocate framebuffers */
  xfb[0] = (u32 *) MEM_K0_TO_K1((u32 *) SYS_AllocateFramebuffer(&TV50hz_576i));
  xfb[1] = (u32 *) MEM_K0_TO_K1((u32 *) SYS_AllocateFramebuffer(&TV50hz_576i));

  /* Define a console */
  console_init(xfb[0], 0, 0, vmode->fbWidth, vmode->xfbHeight, vmode->fbWidth * VI_DISPLAY_PIX_SZ);

  /* Clear framebuffer to black */
  VIDEO_ClearFrameBuffer(vmode, xfb[0], COLOR_BLACK);

  /* Set the framebuffer to be displayed at next VBlank */
  VIDEO_SetNextFramebuffer(xfb[0]);
  fbCurrent = 0;

  /* Enable Video Interface */
  VIDEO_SetBlack(FALSE);

  /* Update VIDEO settings for next VBlank */
  VIDEO_Flush();

  /* Wait for VBlank */
  VIDEO_WaitVSync();
  VIDEO_WaitVSync();

  /* Initialize GX */
  gxStart();
  gxResetRendering(1);
  gxResetMode(vmode, GX_TRUE);
}

void gx_video_Shutdown(void)
{
  FONT_Shutdown();
  VIDEO_ClearFrameBuffer(vmode, xfb[fbCurrent], COLOR_BLACK);
  VIDEO_Flush();
  VIDEO_WaitVSync();
}

/* Custom NTSC blitters */
typedef unsigned short sms_ntsc_out_t;
typedef unsigned short md_ntsc_out_t;

void sms_ntsc_blit( sms_ntsc_t const* ntsc, SMS_NTSC_IN_T const* table, unsigned char* input,
                    int in_width, int vline)
{
  int const chunk_count = in_width / sms_ntsc_in_chunk;

  /* handle extra 0, 1, or 2 pixels by placing them at beginning of row */
  int const in_extra = in_width - chunk_count * sms_ntsc_in_chunk;
  unsigned const extra2 = (unsigned) -(in_extra >> 1 & 1); /* (unsigned) -1 = ~0 */
  unsigned const extra1 = (unsigned) -(in_extra & 1) | extra2;

  /* use palette entry 0 for unused pixels */
  SMS_NTSC_IN_T border = table[0];

  SMS_NTSC_BEGIN_ROW( ntsc, border,
      (SMS_NTSC_ADJ_IN( table[input[0]] )) & extra2,
      (SMS_NTSC_ADJ_IN( table[input[extra2 & 1]] )) & extra1 );

  /* directly fill the RGB565 texture */
  /* one tile is 32 byte = 4x4 pixels */
  /* tiles are stored continuously in texture memory */
  in_width = SMS_NTSC_OUT_WIDTH(in_width) / 4;
  int offset = ((in_width * 32) * (vline / 4)) + ((vline & 3) * 8);
  sms_ntsc_out_t* __restrict__ line_out  = (sms_ntsc_out_t*)(bitmap.data + offset);
  offset = 0;

  int n;
  input += in_extra;

  for ( n = chunk_count; n; --n )
  {
    /* order of input and output pixels must not be altered */
    SMS_NTSC_COLOR_IN( 0, ntsc, SMS_NTSC_ADJ_IN( table[*input++] ) );
    SMS_NTSC_RGB_OUT( 0, line_out[offset++] );
    if ((offset % 4) == 0) offset += 12;
    SMS_NTSC_RGB_OUT( 1, line_out[offset++] );
    if ((offset % 4) == 0) offset += 12;
    
    SMS_NTSC_COLOR_IN( 1, ntsc, SMS_NTSC_ADJ_IN( table[*input++] ) );
    SMS_NTSC_RGB_OUT( 2, line_out[offset++] );
    if ((offset % 4) == 0) offset += 12;
    SMS_NTSC_RGB_OUT( 3, line_out[offset++] );
    if ((offset % 4) == 0) offset += 12;
      
    SMS_NTSC_COLOR_IN( 2, ntsc, SMS_NTSC_ADJ_IN( table[*input++] ) );
    SMS_NTSC_RGB_OUT( 4, line_out[offset++] );
    if ((offset % 4) == 0) offset += 12;
    SMS_NTSC_RGB_OUT( 5, line_out[offset++] );
    if ((offset % 4) == 0) offset += 12;
    SMS_NTSC_RGB_OUT( 6, line_out[offset++] );
    if ((offset % 4) == 0) offset += 12;
  }

  /* finish final pixels */
  SMS_NTSC_COLOR_IN( 0, ntsc, border );
  SMS_NTSC_RGB_OUT( 0, line_out[offset++] );
  if ((offset % 4) == 0) offset += 12;
  SMS_NTSC_RGB_OUT( 1, line_out[offset++] );
  if ((offset % 4) == 0) offset += 12;

  SMS_NTSC_COLOR_IN( 1, ntsc, border );
  SMS_NTSC_RGB_OUT( 2, line_out[offset++] );
  if ((offset % 4) == 0) offset += 12;
  SMS_NTSC_RGB_OUT( 3, line_out[offset++] );
  if ((offset % 4) == 0) offset += 12;

  SMS_NTSC_COLOR_IN( 2, ntsc, border );
  SMS_NTSC_RGB_OUT( 4, line_out[offset++] );
  if ((offset % 4) == 0) offset += 12;
  SMS_NTSC_RGB_OUT( 5, line_out[offset++] );
  if ((offset % 4) == 0) offset += 12;
  SMS_NTSC_RGB_OUT( 6, line_out[offset++] );
  if ((offset % 4) == 0) offset += 12;
}

void md_ntsc_blit( md_ntsc_t const* ntsc, MD_NTSC_IN_T const* table, unsigned char* input,
                   int in_width, int vline)
{
  int const chunk_count = in_width / md_ntsc_in_chunk - 1;

  /* use palette entry 0 for unused pixels */
  MD_NTSC_IN_T border = table[0];

  MD_NTSC_BEGIN_ROW( ntsc, border,
        MD_NTSC_ADJ_IN( table[*input++] ),
        MD_NTSC_ADJ_IN( table[*input++] ),
        MD_NTSC_ADJ_IN( table[*input++] ) );

  /* directly fill the RGB565 texture */
  /* one tile is 32 byte = 4x4 pixels */
  /* tiles are stored continuously in texture memory */
  in_width = MD_NTSC_OUT_WIDTH(in_width) >> 2;
  int offset = ((in_width << 5) * (vline >> 2)) + ((vline & 3) * 8);
  md_ntsc_out_t* __restrict__ line_out  = (md_ntsc_out_t*)(bitmap.data + offset);

  int n;

  for ( n = chunk_count; n; --n )
  {
    /* order of input and output pixels must not be altered */
    MD_NTSC_COLOR_IN( 0, ntsc, MD_NTSC_ADJ_IN( table[*input++] ) );
    MD_NTSC_RGB_OUT( 0, *line_out++ );
    MD_NTSC_RGB_OUT( 1, *line_out++ );

    MD_NTSC_COLOR_IN( 1, ntsc, MD_NTSC_ADJ_IN( table[*input++] ) );
    MD_NTSC_RGB_OUT( 2, *line_out++ );
    MD_NTSC_RGB_OUT( 3, *line_out++ );

    line_out += 12;

    MD_NTSC_COLOR_IN( 2, ntsc, MD_NTSC_ADJ_IN( table[*input++] ) );
    MD_NTSC_RGB_OUT( 4, *line_out++ );
    MD_NTSC_RGB_OUT( 5, *line_out++ );

    MD_NTSC_COLOR_IN( 3, ntsc, MD_NTSC_ADJ_IN( table[*input++] ) );
    MD_NTSC_RGB_OUT( 6, *line_out++ );
    MD_NTSC_RGB_OUT( 7, *line_out++ );

    line_out += 12;
  }

  /* finish final pixels */
  MD_NTSC_COLOR_IN( 0, ntsc, MD_NTSC_ADJ_IN( table[*input++] ) );
  MD_NTSC_RGB_OUT( 0, *line_out++ );
  MD_NTSC_RGB_OUT( 1, *line_out++ );

  MD_NTSC_COLOR_IN( 1, ntsc, border );
  MD_NTSC_RGB_OUT( 2, *line_out++ );
  MD_NTSC_RGB_OUT( 3, *line_out++ );

  line_out += 12;

  MD_NTSC_COLOR_IN( 2, ntsc, border );
  MD_NTSC_RGB_OUT( 4, *line_out++ );
  MD_NTSC_RGB_OUT( 5, *line_out++ );

  MD_NTSC_COLOR_IN( 3, ntsc, border );
  MD_NTSC_RGB_OUT( 6, *line_out++ );
  MD_NTSC_RGB_OUT( 7, *line_out++ );
}
