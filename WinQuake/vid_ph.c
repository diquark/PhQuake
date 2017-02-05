/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2017 diquark

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// vid_ph.c -- Photon video driver

#include <unistd.h>
#include <Ap.h>
#include <Ph.h>
#include <Pt.h>

#include "quakedef.h"
#include "d_local.h"

#define PkIsReleased( f ) ((f & (Pk_KF_Key_Down|Pk_KF_Key_Repeat)) == 0)

unsigned short d_8to16table[256];

static PhImage_t *ph_framebuffer;

static PtWidget_t *Window;
static PtWidget_t *Frame;

extern struct _Ph_ctrl *_Ph_;
static PtAppContext_t app;

//Aplib requires to define ab_exe_path here
char ab_exe_path[PATH_MAX];

static qboolean mouse_avail, mouse_moved;
static float mouse_x, mouse_y;
static int p_mouse_x, p_mouse_y;
#define mouse_buttons 3
static int mouse_oldbuttonstate;
static int mouse_buttonstate;
static int input_group;
static qboolean window_focused = true;

static long Ph_highhunkmark;
static long Ph_buffersize;

static int vid_surfcachesize;
static void *vid_surfcache;

static qboolean palette_changed = true;
static qboolean calc_crc;

typedef struct vidmode_s
{
  const char *description;
  int         width, height;
} vidmode_t;

vidmode_t vid_modes[] =
{
    { "320x240",   320,  240  },
    { "640x480",   640,  480  },
    { "800x600",   800,  600  },
    { "1024x768",  1024, 768  },
    { "1280x1024", 1280, 1024 }
};
#define VID_NUM_MODES ( sizeof( vid_modes ) / sizeof( vid_modes[0] ) )

extern void M_Menu_Options_f (void);
extern void M_Print(int cx, int cy, char *str);
extern void M_PrintWhite(int cx, int cy, char *str);
extern void M_DrawCharacter(int cx, int line, int num);
extern void M_DrawPic(int x, int y, qpic_t *pic);

static qboolean vid_changed;
cvar_t vid_mode = {"vid_mode", "0", false};

static int VID_ChangeMode(int modenum)
{
  if ((modenum >= VID_NUM_MODES) || (modenum < 0))
  {
    Con_Printf("No such video mode %d\n", modenum);
    modenum = 0;
  }
  vid_changed = true;
  Cvar_SetValue("vid_mode", (float)modenum);
  return 1;
}

static int vid_line, vid_column_size;

static void VID_MenuDraw(void)
{
#define MAX_COLUMN_SIZE 11
  qpic_t *p;
  int i, row, column;
  
  p = Draw_CachePic("gfx/vidmodes.lmp");
  M_DrawPic((320-p->width)/2, 4, p);
  
  vid_column_size = (VID_NUM_MODES + 2) / 3;
  
  column = 16;
  row = 36;
  for (i = 0; i < VID_NUM_MODES; i++)
  {
    M_Print(column, row, vid_modes[i].description);
    
    row += 8;
    if ((i % vid_column_size) == (vid_column_size - 1))
    {
      column += 13*8;
      row = 36;
    }
  }
  
  M_Print(9*8, 36 + MAX_COLUMN_SIZE * 8 + 8 * 7,
          "Press Enter to set mode");
  M_Print(15*8, 36 + MAX_COLUMN_SIZE * 8 + 8 * 8,
          "Esc to exit");
  row = 36 + (vid_line % vid_column_size) * 8;
  column = 8 + (vid_line / vid_column_size) * 13*8;

  M_DrawCharacter (column, row, 12+((int)(realtime*4)&1));
}

static void VID_MenuKey(int key)
{
  switch (key)
  {
    case K_ESCAPE:
      S_LocalSound("misc/menu1.wav");
      M_Menu_Options_f();
      break;
    case K_UPARROW:
      S_LocalSound ("misc/menu1.wav");
      vid_line--;

      if (vid_line < 0)
        vid_line = VID_NUM_MODES - 1;
      break;
    case K_DOWNARROW:
      S_LocalSound ("misc/menu1.wav");
      vid_line++;

      if (vid_line >= VID_NUM_MODES)
        vid_line = 0;
      break;
    case K_LEFTARROW:
      S_LocalSound ("misc/menu1.wav");
      vid_line -= vid_column_size;

      if (vid_line < 0)
      {
        vid_line += ((VID_NUM_MODES + (vid_column_size - 1)) /
					vid_column_size) * vid_column_size;

        while (vid_line >= VID_NUM_MODES)
          vid_line -= vid_column_size;
      }
      break;
    case K_RIGHTARROW:
      S_LocalSound ("misc/menu1.wav");
      vid_line += vid_column_size;

      if (vid_line >= VID_NUM_MODES)
      {
        vid_line -= ((VID_NUM_MODES + (vid_column_size - 1)) /
        vid_column_size) * vid_column_size;

			  while (vid_line < 0)
				  vid_line += vid_column_size;
		  }
      break;
    case K_ENTER:
      S_LocalSound ("misc/menu1.wav");
      VID_ChangeMode (vid_line);
      break;
    default:
      break;
  }
}

typedef enum
{
  cursor_hide,
  cursor_display
} cursortype_t;

static void ChangeCursorAppearance(cursortype_t cursortype)
{
  PtArg_t arg;
  
  PtSetArg(&arg, Pt_ARG_CURSOR_TYPE, 
           (cursortype == cursor_hide) ? Ph_CURSOR_NONE : Ph_CURSOR_INHERIT, 0);
  PtSetResources(Window, 1, &arg);
}

static void MoveCursorToCenter(void)
{
  short x, y;

  PtGetAbsPosition(Window, &x, &y);
  p_mouse_x = x + Window->area.size.w / 2;
  p_mouse_y = y + Window->area.size.h / 2;
  PhMoveCursorAbs(input_group, p_mouse_x, p_mouse_y);
}

void VID_SetPalette (unsigned char *palette)
{
  int i;
  
  for (i=0 ; i < 256 ; i++)
    ph_framebuffer->palette[i] = PgRGB(palette[i*3], palette[i*3 + 1], palette[i*3 + 2]);
  palette_changed = true;
}

void VID_ShiftPalette (unsigned char *palette)
{
  VID_SetPalette (palette);
}

static void DrawFrame(PtWidget_t *widget, PhTile_t *damage)
{
  PhDim_t size;
  PhGC_t *GC;
  PhPoint_t pos;
  size_t image_offset;
  static long pal_tag;
  
  //PgDrawPhImagemx(&pos, ph_framebuffer, 0);
  
  if (calc_crc)
    ph_framebuffer->image_tag = PxCRC(ph_framebuffer->image,
              ph_framebuffer->bpl * ph_framebuffer->size.h);
 
  if (palette_changed)
  {
    ph_framebuffer->palette_tag = PxCRC((char*)ph_framebuffer->palette,
                           sizeof(PgColor_t) * ph_framebuffer->colors);
    pal_tag = ph_framebuffer->palette_tag;
    palette_changed = false;
  }
  
  size.w = damage->rect.lr.x - damage->rect.ul.x + 1;
  size.h = damage->rect.lr.y - damage->rect.ul.y + 1;
  pos    = damage->rect.ul;
  
  GC = PgGetGC();
  
  if (GC && GC->palette_tag != pal_tag)
    PgSetPalette(ph_framebuffer->palette, 0, 0, ph_framebuffer->colors, 
                 Pg_PALSET_SOFT, ph_framebuffer->palette_tag);
                                  
  image_offset = pos.y * ph_framebuffer->bpl + pos.x;
  PgDrawImagemx(&ph_framebuffer->image[image_offset],
                ph_framebuffer->type,
                &pos,
                &size,
                ph_framebuffer->bpl,
                ph_framebuffer->image_tag);
  
}

static PtWidget_t* LoadIcon(void)
{
  ApDBase_t *dbase;
  PtWidget_t *icon  = NULL;
  
  dbase = ApOpenExecDBaseFile(com_argv[0], "Icon.wgti");
  if (dbase)
  {
    ApAddClass("PtIcon", &PtIcon);
    ApAddClass("PtLabel", &PtLabel);
    icon = ApCreateWidgetFamily(dbase, "Icon", 0, 0, 0, NULL);
    // Don't close dbase: there are few PhImage_t data attached to widgets
    //ApCloseDBase(dbase);
  }
  return icon;
}

static void ResetFrameBuffer(void)
{
  static PgColor_t *palette;
  if (ph_framebuffer)
  {
    PhReleaseImage(ph_framebuffer);
    free(ph_framebuffer);
  }

  if (d_pzbuffer)
  {
    D_FlushCaches ();
    Hunk_FreeToHighMark (Ph_highhunkmark);
    d_pzbuffer = NULL;
  }
  Ph_highhunkmark = Hunk_HighMark ();
  
// alloc an extra line in case we want to wrap, and allocate the z-buffer

  Ph_buffersize = vid.width * vid.height * sizeof (*d_pzbuffer);

  vid_surfcachesize = D_SurfaceCacheForRes (vid.width, vid.height);

  Ph_buffersize += vid_surfcachesize;

  d_pzbuffer = Hunk_HighAllocName (Ph_buffersize, "video");
  if (d_pzbuffer == NULL)
    Sys_Error ("VID: Not enough memory for video mode\n");

  vid_surfcache = (byte *) d_pzbuffer
    + vid.width * vid.height * sizeof (*d_pzbuffer);

  D_InitCaches(vid_surfcache, vid_surfcachesize);


/*
  d_pzbuffer = zbuffer;
  D_InitCaches (surfcache, sizeof(surfcache));
*/

  ph_framebuffer = (PhImage_t*)calloc(1, sizeof(*ph_framebuffer));
  if (!ph_framebuffer)
    Sys_Error("VID: Could not allocate memory for image header");
  ph_framebuffer->flags = Ph_RELEASE_IMAGE;
  ph_framebuffer->type = Pg_IMAGE_PALETTE_BYTE;
  ph_framebuffer->size.w = vid.width;
  ph_framebuffer->size.h = vid.height;
  ph_framebuffer->bpl = ph_framebuffer->size.w;
  ph_framebuffer->image = (char*)PgShmemCreate(ph_framebuffer->bpl * ph_framebuffer->size.h, NULL);
  if (!ph_framebuffer->image)
    Sys_Error("VID: Could not allocate shared memory for image pixel data");
  ph_framebuffer->colors = 256;
  if (!palette)
  {
    palette = (PgColor_t*)calloc(ph_framebuffer->colors, sizeof(PgColor_t));
    if (!palette)
      Sys_Error("VID: Could not allocate memory for image palette");
  }
  ph_framebuffer->palette = palette;
  
  vid.buffer = (pixel_t*) (ph_framebuffer->image);
  vid.conbuffer = vid.buffer;
  vid.rowbytes = ph_framebuffer->bpl;
  vid.conrowbytes = vid.rowbytes;
  vid.conwidth = vid.width;
  vid.conheight = vid.height;
  vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
  //vid.aspect = 1;
  vid.direct = 0;
}

void VID_Init (unsigned char *palette)
{
  char *displayname = NULL;
  PhChannelParms_t parms = {0, 0, Ph_DYNAMIC_BUFFER};
  PhDim_t dim;
  PtArg_t arg[7];
  PtWidget_t *Icon;
  PhRid_t rid;
  PhRegion_t region;
  char *env_var;
  int pnum;
  
  vid.width = vid_modes[0].width;
  vid.height = vid_modes[0].height;
  vid.maxwarpwidth = WARP_WIDTH;
  vid.maxwarpheight = WARP_HEIGHT;
  vid.numpages = 1;
  vid.colormap = host_colormap;
  vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

  srand(getpid());
  
  // check if CRC for image data should be calculated
  if (COM_CheckParm("-crc"))
    calc_crc = true;
  
  // check for command-line display name
  if ((pnum=COM_CheckParm("-disp")) != 0 && pnum < com_argc - 1)
    displayname = com_argv[pnum+1];

  // open the display
  if (!PhAttach(displayname, &parms))
  {
    if (displayname)
      Sys_Error("VID: Could not attach to Photon manager [%s]", displayname);
    else if ((displayname = getenv("PHOTON")))
      Sys_Error("VID: Could not attach to Photon manager (PHOTON=[%s])", displayname);
    else
      Sys_Error("VID: Could not attach to Photon manager [/dev/photon]");
  }
  
  Cvar_RegisterVariable(&vid_mode);
  
  // check for command-line window size
  if ((pnum=COM_CheckParm("-winsize")))
  {
    if (pnum >= com_argc-2)
      Sys_Error("VID: -winsize <width> <height>\n");
    vid.width = Q_atoi(com_argv[pnum+1]);
    vid.height = Q_atoi(com_argv[pnum+2]);
    if (!vid.width || !vid.height)
      Sys_Error("VID: Bad window width/height\n");
  }
  if ((pnum=COM_CheckParm("-width")))
  {
    if (pnum >= com_argc-1)
      Sys_Error("VID: -width <width>\n");
    vid.width = Q_atoi(com_argv[pnum+1]);
    if (!vid.width)
      Sys_Error("VID: Bad window width\n");
  }
  if ((pnum=COM_CheckParm("-height")))
  {
    if (pnum >= com_argc-1)
      Sys_Error("VID: -height <height>\n");
    vid.height = Q_atoi(com_argv[pnum+1]);
    if (!vid.height)
      Sys_Error("VID: Bad window height\n");
  }
  
  ResetFrameBuffer();

  PgSetDrawBufferSize(USHRT_MAX);
  
  PtInit(NULL);
  Icon = LoadIcon();
  
  dim.w = vid.width;
  dim.h = vid.height;
  
  PtSetArg(&arg[0], Pt_ARG_WINDOW_TITLE, "PhQuake", 0);
  PtSetArg(&arg[1], Pt_ARG_ICON_WINDOW, Icon, 0);
  PtSetArg(&arg[2], Pt_ARG_CURSOR_TYPE, Ph_CURSOR_NONE, 0);
  PtSetArg(&arg[3], Pt_ARG_WINDOW_RENDER_FLAGS,
                    Ph_WM_RENDER_ASAPP |
                    Ph_WM_RENDER_CLOSE |
                    Ph_WM_RENDER_TITLE |
                    Ph_WM_RENDER_MIN,
                    Pt_TRUE);
  PtSetArg(&arg[4], Pt_ARG_WINDOW_NOTIFY_FLAGS,
                    Ph_WM_CLOSE | Ph_WM_FOCUS,
                    Pt_TRUE);
  PtSetArg(&arg[5], Pt_ARG_DIM, &dim, 0);
  PtSetArg(&arg[6], Pt_ARG_RESIZE_FLAGS, 0, Pt_TRUE);

  PtSetParentWidget(NULL);
  Window = PtCreateWidget(PtWindow, NULL, 7, arg);
  PtReParentWidget(Icon, Window);
  
  PtSetArg(&arg[0], Pt_ARG_DIM, &dim, 0);
  PtSetArg(&arg[1], Pt_ARG_RAW_DRAW_F, DrawFrame, 0);
  
  Frame = PtCreateWidget(PtRaw, Window, 2, arg);
  PtRealizeWidget(Window);
  
  // Make Window region sensetive to mouse movements
  rid = PtWidgetRid(Window);
  PhRegionQuery(rid, &region, NULL, NULL, 0);
  region.events_sense |= Ph_EV_PTR_MOTION_BUTTON | Ph_EV_PTR_MOTION_NOBUTTON;
  PhRegionChange(Ph_REGION_EV_SENSE, Ph_EXPOSE_REGION, &region, NULL, NULL);

  if (!((env_var = getenv("PHIG")) && (input_group = atoi(env_var))))
    input_group = 1;
  MoveCursorToCenter();
  
  app = PtDefaultAppContext();
  
  vid_menudrawfn = VID_MenuDraw;
  vid_menukeyfn = VID_MenuKey;
}

//
//  Translates the key
//  Use keycap, becase keysym is not valid on release
//
static int translatekey(unsigned long keycap)
{
    int rc;
    
    switch(keycap)
    {
      case Pk_Pg_Up: rc = K_PGUP; break;
      case Pk_Pg_Down: rc = K_PGDN; break;
      case Pk_Home: rc = K_HOME; break;
      case Pk_End: rc = K_END; break;
      case Pk_Left: rc = K_LEFTARROW; break;
      case Pk_Right: rc = K_RIGHTARROW; break;
      case Pk_Down: rc = K_DOWNARROW; break;
      case Pk_Up: rc = K_UPARROW; break;
      case Pk_Escape: rc = K_ESCAPE; break;
      case Pk_KP_Enter:
      case Pk_Return: rc = K_ENTER; break;
      case Pk_Tab: rc = K_TAB; break;
      
      case Pk_F1: rc = K_F1; break;
      case Pk_F2: rc = K_F2; break;
      case Pk_F3: rc = K_F3; break;
      case Pk_F4: rc = K_F4; break;
      case Pk_F5: rc = K_F5; break;
      case Pk_F6: rc = K_F6; break;
      case Pk_F7: rc = K_F7; break;
      case Pk_F8: rc = K_F8; break;
      case Pk_F9: rc = K_F9; break;
      case Pk_F10: rc = K_F10; break;
      case Pk_F11: rc = K_F11; break;
      case Pk_F12: rc = K_F12; break;

      case Pk_BackSpace: rc = K_BACKSPACE; break;
      case Pk_Delete: rc = K_DEL; break;

      case Pk_Pause: rc = K_PAUSE; break;

      case Pk_Shift_L:
      case Pk_Shift_R: rc = K_SHIFT; break;
        
      case Pk_Execute: 
      case Pk_Control_L: 
      case Pk_Control_R:	rc = K_CTRL;		 break;
      
      case Pk_Alt_L:
      case Pk_Meta_L:
      case Pk_Alt_R:
      case Pk_Meta_R: rc = K_ALT; break;

      case Pk_Insert: rc = K_INS; break;
      
      case Pk_KP_Multiply: rc = '*'; break;
      case Pk_KP_Add: rc = '+'; break;
      case Pk_KP_Subtract: rc = '-'; break;
      case Pk_KP_Divide: rc = '/'; break;

      default:
        rc = keycap;
        if (rc >= 'A' && rc <= 'Z')
          rc = rc - 'A' + 'a';
        break;
    }
    return rc;
}

static void ClearAllStates(void)
{
  int i;
  
  for (i=0; i < 256; i++)
    Key_Event(i, false);
  
  Key_ClearStates();
  mouse_oldbuttonstate = 0;
}

static struct
{
  int key;
  int down;
} keyq[64];
static int keyq_head;
static int keyq_tail;

static void GetEvent(void)
{
  union
  {
    void *raw;
    PhKeyEvent_t *key_ev;
    PhPointerEvent_t *ptr_ev;
    PhWindowEvent_t  *win_ev;
  } ph_ev;
  
  app->event->processing_flags = 0;
  ph_ev.raw = PhGetData(app->event);
  switch(app->event->type)
  {
    case Ph_EV_KEY:
      if (PkIsFirstDown(ph_ev.key_ev->key_flags))
      {
        keyq[keyq_head].key = translatekey(ph_ev.key_ev->key_cap);
        keyq[keyq_head].down = true;
        keyq_head = (keyq_head + 1) & 63;
      }
      else if (PkIsReleased(ph_ev.key_ev->key_flags))
      {
        keyq[keyq_head].key = translatekey(ph_ev.key_ev->key_cap);
        keyq[keyq_head].down = false;
        keyq_head = (keyq_head + 1) & 63;
      }
      break;
    case Ph_EV_BUT_RELEASE:
      if (app->event->subtype == Ph_EV_RELEASE_ENDCLICK)
        break;
    case Ph_EV_BUT_PRESS:
      mouse_buttonstate = (ph_ev.ptr_ev->button_state & Ph_BUTTON_SELECT ? 1 : 0)
                        | (ph_ev.ptr_ev->button_state & Ph_BUTTON_MENU   ? 2 : 0)
                        | (ph_ev.ptr_ev->button_state & Ph_BUTTON_ADJUST ? 4 : 0);
      break;
    case Ph_EV_BOUNDARY:
      if (app->event->subtype != Ph_EV_PTR_LEAVE)
        break;
    case Ph_EV_PTR_MOTION_BUTTON:
      mouse_buttonstate = (ph_ev.ptr_ev->button_state & Ph_BUTTON_SELECT ? 1 : 0)
                        | (ph_ev.ptr_ev->button_state & Ph_BUTTON_MENU   ? 2 : 0)
                        | (ph_ev.ptr_ev->button_state & Ph_BUTTON_ADJUST ? 4 : 0);
    case Ph_EV_PTR_MOTION_NOBUTTON:
      if (window_focused)
      {
        mouse_x = ph_ev.ptr_ev->pos.x - p_mouse_x;
        mouse_y = ph_ev.ptr_ev->pos.y - p_mouse_y;
        
        p_mouse_x = ph_ev.ptr_ev->pos.x;
        p_mouse_y = ph_ev.ptr_ev->pos.y;
        
        mouse_moved = true;
      }
      break;
    case Ph_EV_WM:
      if (ph_ev.win_ev->event_f & Ph_WM_FOCUS)
      {
        window_focused = (ph_ev.win_ev->event_state ==
                          Ph_WM_EVSTATE_FOCUSLOST) ? false : true;
        if (!window_focused && (ph_ev.win_ev->state_f & Ph_WM_STATE_ISFOCUS))
        {
          //Window lost focus
          ClearAllStates();
          ChangeCursorAppearance(cursor_display);
        }
        else if (window_focused && !(ph_ev.win_ev->state_f & Ph_WM_STATE_ISFOCUS))
        {
          //Window get focus
          ChangeCursorAppearance(cursor_hide);
        }  
      }
      if (ph_ev.win_ev->event_f & Ph_WM_RESIZE)
      {
        printf("%hu %hu\n", ph_ev.win_ev->size.w, ph_ev.win_ev->size.h);
      }
      break;
  }
  PtEventHandler(app->event);
  if (_Pt_->destroyed_widgets)
    PtRemoveWidget();
}

void VID_Shutdown (void)
{
  if (ph_framebuffer)
  {
    if(ph_framebuffer->palette)
    {
      free(ph_framebuffer->palette);
      ph_framebuffer->palette = NULL;
    }
    PhReleaseImage(ph_framebuffer);
    free(ph_framebuffer);
    ph_framebuffer = NULL;
  }
  // Remove shared memory references;
  PgShmemCleanup();
}

void VID_Update (vrect_t *rects)
{
  PhRect_t extent;
  
  //PtDamageWidget(Frame);
  
  if (vid_changed)
  {
    PtArg_t arg;
    PhDim_t dim;
    
    vid_changed = false;
    vid.width = vid_modes[(int)vid_mode.value].width;
    vid.height = vid_modes[(int)vid_mode.value].height;
    ResetFrameBuffer();
    vid.recalc_refdef = 1;
    Con_CheckResize();
    Con_Clear_f();
    
    dim.w = vid.width;
    dim.h = vid.height;
    PtSetArg(&arg, Pt_ARG_DIM, &dim, 0);
    PtSetResources(Window, 1, &arg);
    
    PtSetArg(&arg, Pt_ARG_DIM, &dim, 0);
    PtSetResources(Frame, 1, &arg);
    
    MoveCursorToCenter();
    return;
  }
  
  if (palette_changed)
  {
    extent.ul.x = 0;
    extent.lr.x = vid.width - 1;
    extent.ul.y = 0;
    extent.lr.y = vid.height - 1;
    PtDamageExtent(Frame, &extent);
  }
  else
    while (rects)
    {
      extent.ul.x = rects->x;
      extent.lr.x = extent.ul.x + rects->width - 1;
      extent.ul.y = rects->y;
      extent.lr.y = extent.ul.y + rects->height - 1;
      PtDamageExtent(Frame, &extent);

      rects = rects->pnext;
    }
  PtFlush();
}

/*
================
D_BeginDirectRect
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}


/*
================
D_EndDirectRect
================
*/
void D_EndDirectRect (int x, int y, int width, int height)
{
}

void Sys_SendKeyEvents(void)
{
  int ret;
  
  if (_Ph_)
  {
    while((ret = PhEventPeek(app->event, app->event_size)))
    {
      switch(ret)
      {
        case Ph_EVENT_MSG:
          GetEvent();
          break;
        case Ph_RESIZE_MSG:
          if (PtResizeEventMsg(app, PhGetMsgSize(app->event)) == -1)
            Sys_Error("Can not reallocate event buffer");
          break;
        case -1:
          Sys_Error("Receiving Photon event");
          break;
      }
    }
    while (keyq_head != keyq_tail)
    {
      Key_Event(keyq[keyq_tail].key, keyq[keyq_tail].down);
      if (keyq_head == keyq_tail) //Bug???
        break;
      keyq_tail = (keyq_tail + 1) & 63;
    }
    if (window_focused && mouse_moved)
      MoveCursorToCenter();
    
    mouse_moved = false;
  }
}

void IN_Commands(void)
{
  int i;

  if (!mouse_avail)
    return;

  for (i=0 ; i<mouse_buttons ; i++)
  {
    if ( (mouse_buttonstate & (1<<i)) && !(mouse_oldbuttonstate & (1<<i)) )
      Key_Event (K_MOUSE1 + i, true);

    if ( !(mouse_buttonstate & (1<<i)) && (mouse_oldbuttonstate & (1<<i)) )
      Key_Event (K_MOUSE1 + i, false);
  }
  mouse_oldbuttonstate = mouse_buttonstate;
}

void IN_Move(usercmd_t *cmd)
{
  float mx, my;
  
  if (!mouse_avail || !window_focused)
    return;

  mx = mouse_x * sensitivity.value;
  my = mouse_y * sensitivity.value;
   
  if ( (in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1) ))
    cmd->sidemove += m_side.value * mx;
  else
    cl.viewangles[YAW] -= m_yaw.value * mx;
  if (in_mlook.state & 1)
    V_StopPitchDrift ();
   
  if ( (in_mlook.state & 1) && !(in_strafe.state & 1))
  {
    cl.viewangles[PITCH] += m_pitch.value * my;
    if (cl.viewangles[PITCH] > 80)
      cl.viewangles[PITCH] = 80;
    if (cl.viewangles[PITCH] < -70)
      cl.viewangles[PITCH] = -70;
  }
  else
  {
    if ((in_strafe.state & 1) && noclip_anglehack)
      cmd->upmove -= m_forward.value * my;
    else
      cmd->forwardmove -= m_forward.value * my;
  }
  mouse_x = mouse_y = 0.0;
}

void IN_Init(void)
{
  if ( COM_CheckParm ("-nomouse") )
    return;
  mouse_x = mouse_y = 0.0;
  mouse_avail = true;
}

void IN_Shutdown(void)
{
  mouse_avail = false;
}
