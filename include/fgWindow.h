// Copyright �2013 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in "feathergui.h"

#ifndef __FG_WINDOW_H__
#define __FG_WINDOW_H__

#include "fgStatic.h"

#ifdef  __cplusplus
extern "C" {
#endif

enum FG_WINFLAGS
{
  FGWIN_NOCLIP=1,
  FGWIN_HIDDEN=2,
  FGWIN_EXPANDX=4,
  FGWIN_EXPANDY=8,
  FGGRID_TILEX=16,
  FGGRID_TILEY=32
};

struct FG_MENU;

// Defines the base GUI element, a window. This is not an actual top level window.
typedef struct __WINDOW {
  fgChild element;
  char (FG_FASTCALL *message)(void* self, const FG_Msg* msg);
  FG_UINT id;
  unsigned char flags; // 1 is clipping disabled, 2 is not visible
  fgStatic* rlist; // root node for statics
  fgStatic* rlast; // last node for statics 
  struct FG_MENU* contextmenu;
} fgWindow;

MAKE_VECTOR(fgWindow*,VectWindow);

FG_EXTERN fgWindow* fgFocusedWindow;
FG_EXTERN fgWindow* fgLastHover; // Last window the mouse moved over, used to generate MOUSEON and MOUSEOFF events
FG_EXTERN VectWindow fgNonClipping;

FG_EXTERN void FG_FASTCALL fgWindow_Init(fgWindow* BSS_RESTRICT self, fgWindow* BSS_RESTRICT parent);
FG_EXTERN void FG_FASTCALL fgWindow_Destroy(fgWindow* self);
FG_EXTERN char FG_FASTCALL fgWindow_Message(fgWindow* self, const FG_Msg* msg);
FG_EXTERN void FG_FASTCALL fgWindow_SetElement(fgWindow* self, fgElement* element);
FG_EXTERN void FG_FASTCALL fgWindow_SetArea(fgWindow* self, CRect* area);
FG_EXTERN void FG_FASTCALL fgWindow_BasicMessage(fgWindow* self, unsigned char type); // Shortcut for sending type messages with no data
FG_EXTERN void FG_FASTCALL fgWindow_VoidMessage(fgWindow* self, unsigned char type, void* data); // Shortcut for sending void* messages
FG_EXTERN void FG_FASTCALL fgWindow_IntMessage(fgWindow* self, unsigned char type, int data); // Shortcut for sending int messages
FG_EXTERN void FG_FASTCALL DoSkinCheck(fgWindow* self, fgStatic** skins, const FG_Msg* msg);

#ifdef  __cplusplus
}
#endif

#endif
