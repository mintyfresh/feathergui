// Copyright �2017 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in "feathergui.h"

#include "fgElement.h"
#include "fgRoot.h"
#include "fgLayout.h"
#include "feathercpp.h"
#include "bss-util/khash.h"
#include <math.h>
#include <limits.h>

KHASH_INIT(fgUserdata, char*, size_t, 1, kh_str_hash_func, kh_str_hash_equal);
KHASH_INIT(fgSkinElements, fgElement*, char, 0, kh_ptr_hash_func, kh_int_hash_equal);

template<typename U, typename V>
BSS_FORCEINLINE char CompPairInOrder(const std::pair<U, V>& l, const std::pair<U, V>& r) { char ret = SGNCOMPARE(l.first, r.first); return !ret ? SGNCOMPARE(l.second, r.second) : ret; }

typedef bss_util::cDynArray<fgElement*, FG_UINT> fgSkinRefArray;
bss_util::cAVLtree<std::pair<fgElement*, unsigned short>, void, &CompPairInOrder> fgListenerList;

void FG_FASTCALL fgElement_InternalSetup(fgElement* BSS_RESTRICT self, fgElement* BSS_RESTRICT parent, fgElement* BSS_RESTRICT next, const char* name, fgFlag flags, const fgTransform* transform, unsigned short units, void (MSC_FASTCALL *GCC_FASTCALL destroy)(void*), size_t(MSC_FASTCALL *GCC_FASTCALL message)(void*, const FG_Msg*))
{
  assert(self != 0);
  memset(self, 0, sizeof(fgElement));
  self->destroy = destroy;
  self->free = 0;
  self->name = fgCopyText(name, __FILE__, __LINE__);
  self->message = message;
  self->flags = flags & (~FGELEMENT_USEDEFAULTS);
  self->style = (FG_UINT)-1;
  self->maxdim.x = -1.0f;
  self->maxdim.y = -1.0f;
  self->mindim.x = -1.0f;
  self->mindim.y = -1.0f;
  self->scaling.x = 1.0f;
  self->scaling.y = 1.0f;
  if(transform)
  {
    self->transform = *transform;
    if(units != 0 && units != (unsigned short)-1)
    {
      fgResolveCRectUnit(self, self->transform.area, units);
      fgResolveCVecUnit(self, self->transform.center, units);
    }
  }
  _sendmsg<FG_CONSTRUCT>(self);
  _sendmsg<FG_SETPARENT, void*, void*>(self, parent, next);
}

void FG_FASTCALL fgElement_Init(fgElement* BSS_RESTRICT self, fgElement* BSS_RESTRICT parent, fgElement* BSS_RESTRICT next, const char* name, fgFlag flags, const fgTransform* transform, unsigned short units)
{
  fgElement_InternalSetup(self, parent, next, name, flags, transform, units, (fgDestroy)&fgElement_Destroy, (fgMessage)&fgElement_Message);
}

void FG_FASTCALL fgElement_Destroy(fgElement* self)
{
  assert(self != 0);
  fgRoot_RemoveID(fgroot_instance, self);
  _sendmsg<FG_DESTROY>(self);
  if(fgFocusedWindow == self) // We first try to bump focus up to our parents
  {
    fgFocusedWindow = 0;
    if(self->parent != 0)
      _sendmsg<FG_GOTFOCUS, void*>(self->parent, self);
  }
  if(self->parent != 0)
    _sendmsg<FG_REMOVECHILD, void*>(self->parent, self);
  self->parent = 0;
  fgElement_Clear(self);

  if(fgFocusedWindow == self) // There are some known cases where a child might have focus and destroying it bumps focus back up to us.
    fgFocusedWindow = 0; // However, we must detach ourselves from our parents before we clear out our elements, so any focus buried inside our element is simply lost.
  if(fgLastHover == self)
    fgLastHover = 0;
  if(fgCaptureWindow == self)
    fgCaptureWindow = 0;

  if(self->userhash)
  {
    for(khiter_t i = 0; i < self->userhash->n_buckets; ++i)
      if(kh_exist(self->userhash, i))
        fgfree(kh_key(self->userhash, i), __FILE__, __LINE__);
    kh_destroy_fgUserdata(self->userhash);
    self->userhash = 0;
  }
  if(self->name)
    fgfree(self->name, __FILE__, __LINE__);
  if(self->skinelements != 0)
    kh_destroy_fgSkinElements(self->skinelements);
  fgElement_ClearListeners(self);
  assert(fgFocusedWindow != self); // If these assertions fail something is wrong with how the message chain is constructed
  assert(fgLastHover != self);
  assert(fgCaptureWindow != self);
}

// (1<<1) resize x (2)
// (1<<2) resize y (4)
// (1<<3) move x (8)
// (1<<4) move y (16)

#ifdef BSS_DEBUG
bool FG_FASTCALL fgElement_VERIFY(fgElement* self)
{
  for(khiter_t i = 0; i < self->userhash->n_buckets; ++i)
    if(kh_exist(self->userhash, i))
      if(!fgLeakTracker::Tracker.Verify(kh_key(self->userhash, i)))
        return false;
  return true;
}
#endif

BSS_FORCEINLINE fgElement*& fgElement_prev(fgElement* p) { return p->prev; }
BSS_FORCEINLINE fgElement*& fgElement_previnject(fgElement* p) { return p->previnject; }
BSS_FORCEINLINE fgElement*& fgElement_prevnoclip(fgElement* p) { return p->prevnoclip; }

BSS_FORCEINLINE fgElement*& fgElement_next(fgElement* p) { return p->next; }
BSS_FORCEINLINE fgElement*& fgElement_nextinject(fgElement* p) { return p->nextinject; }
BSS_FORCEINLINE fgElement*& fgElement_nextnoclip(fgElement* p) { return p->nextnoclip; }

template<fgElement*&(*PREV)(fgElement*), fgElement*&(*NEXT)(fgElement*)>
inline void FG_FASTCALL LList_Insert(fgElement* self, fgElement* cur, fgElement* prev, fgElement** root, fgElement** last)
{
  NEXT(self) = cur;
  PREV(self) = prev;
  if(prev) NEXT(prev) = self;
  else *root = self; // Prev is only null if we're inserting before the root, which means we must reassign the root.
  if(cur) PREV(cur) = self;
  else *last = self; // Cur is null if we are at the end of the list, so update last
}

template<fgElement*&(*GET)(fgElement*), fgFlag FLAG>
inline fgElement* FG_FASTCALL LList_Find(fgElement* BSS_RESTRICT self)
{
  fgFlag flags = self->flags;
  do
  {
    self = GET(self);
  } while(self && (self->flags&FLAG) != (flags&FLAG));
  return self;
}

inline void FG_FASTCALL LList_InsertAll(fgElement* BSS_RESTRICT self, fgElement* BSS_RESTRICT next)
{
  assert(self->parent != 0);
  fgElement* prev = !next ? self->parent->last : next->prev;
  LList_Insert<fgElement_prev, fgElement_next>(self, next, prev, &self->parent->root, &self->parent->last);
  if(!(self->flags&FGELEMENT_IGNORE))
  {
    prev = LList_Find<fgElement_prev, FGELEMENT_IGNORE>(self); // Ensure that prev and next are appropriately set to elements that match our clipping status.
    next = LList_Find<fgElement_next, FGELEMENT_IGNORE>(self); // we DO NOT use fgElement_prevclip or fgElement_nextclip here because they haven't been set yet.
    LList_Insert<fgElement_previnject, fgElement_nextinject>(self, next, prev, &self->parent->rootinject, &self->parent->lastinject);
    fgElement_MouseMoveCheck(self); // This has to be AFTER so we can properly resolve relative coordinates
  }
  if(self->flags&FGELEMENT_NOCLIP)
  {
    prev = LList_Find<fgElement_prev, FGELEMENT_NOCLIP>(self);
    next = LList_Find<fgElement_next, FGELEMENT_NOCLIP>(self);
    LList_Insert<fgElement_prevnoclip, fgElement_nextnoclip>(self, next, prev, &self->parent->rootnoclip, &self->parent->lastnoclip);
  }
}

template<fgElement*&(*PREV)(fgElement*), fgElement*&(*NEXT)(fgElement*)>
inline void FG_FASTCALL LList_Remove(fgElement* self, fgElement** root, fgElement** last)
{
  assert(self != 0);
  if(PREV(self) != 0) NEXT(PREV(self)) = NEXT(self);
  else *root = NEXT(self);
  if(NEXT(self) != 0) PREV(NEXT(self)) = PREV(self);
  else *last = PREV(self);
}

inline void FG_FASTCALL LList_RemoveAll(fgElement* self)
{
  assert(self->parent != 0);
  LList_Remove<fgElement_prev, fgElement_next>(self, &self->parent->root, &self->parent->last); // Remove ourselves from our parent
  if(!(self->flags&FGELEMENT_IGNORE))
  {
    fgElement_MouseMoveCheck(self); // we have to check if the mouse intersects the child BEFORE we remove it so we can properly resolve relative coordinates.
    LList_Remove<fgElement_previnject, fgElement_nextinject>(self, &self->parent->rootinject, &self->parent->lastinject);
  }
  if(self->flags&FGELEMENT_NOCLIP)
    LList_Remove<fgElement_prevnoclip, fgElement_nextnoclip>(self, &self->parent->rootnoclip, &self->parent->lastnoclip); // Remove ourselves from our parent
}

char FG_FASTCALL fgElement_PotentialResize(fgElement* self)
{
  return ((self->transform.area.left.rel != 0 || self->transform.area.right.rel != 0) << 3) // If you have nonzero relative coordinates, a resize will cause a move
    | ((self->transform.area.top.rel != 0 || self->transform.area.bottom.rel != 0) << 4)
    | ((self->transform.area.left.rel != self->transform.area.right.rel) << 1) // If you have DIFFERENT relative coordinates, a resize will cause a move AND a resize for you.
    | ((self->transform.area.top.rel != self->transform.area.bottom.rel) << 2);
}

fgElement* FG_FASTCALL fgElement_LoadLayout(fgElement* parent, fgElement* next, fgClassLayout* layout)
{
  fgElement* element = fgroot_instance->backend.fgCreate(layout->style.type, parent, next, layout->style.name, layout->style.flags, (layout->style.units == -1) ? 0 : &layout->style.transform, layout->style.units);
  if(layout->style.id != 0)
    fgRoot_AddID(fgroot_instance, layout->style.id, element);
  _sendsubmsg<FG_SETSTYLE, void*, size_t>(element, FGSETSTYLE_POINTER, &layout->style.style, ~0);
  fgroot_instance->backend.fgUserDataMap(element, &layout->userdata); // Map any custom userdata to this element

  for(FG_UINT i = 0; i < layout->children.l; ++i)
    fgElement_LoadLayout(element, 0, layout->children.p + i);

  return element;
}

void FG_FASTCALL fgElement_ApplySkin(fgElement* self, const fgSkin* skin)
{
  if(skin->inherit) // apply inherited skin first so we override it.
    fgElement_ApplySkin(self, skin->inherit);

  fgElement* child = self->root;
  for(FG_UINT i = 0; i < skin->children.l; ++i)
  {
    fgStyleLayout* layout = skin->children.p + i;
    child = fgroot_instance->backend.fgCreate(layout->type, self, child, layout->name, layout->flags, (layout->units == -1) ? 0 : &layout->transform, layout->units);
    assert(child != 0);
    _sendsubmsg<FG_SETSTYLE, void*, size_t>(child, FGSETSTYLE_POINTER, &layout->style, ~0);
    if(!self->skinelements)
      self->skinelements = kh_init_fgSkinElements();
    int r;
    kh_put_fgSkinElements(self->skinelements, child, &r);
    assert(r != 0);
  }

  _sendsubmsg<FG_SETSTYLE, void*, size_t>(self, FGSETSTYLE_POINTER, (void*)&self->skin->style, ~0);
}


size_t FG_FASTCALL fgElement_CheckLastFocus(fgElement* self)
{
  if(self->lastfocus)
  {
    fgElement* hold = self->lastfocus;
    self->lastfocus = 0;
    if(_sendmsg<FG_GOTFOCUS>(hold))
      return 1;
  }
  return 0;
}

size_t FG_FASTCALL fgElement_Message(fgElement* self, const FG_Msg* msg)
{
  ptrdiff_t otherint = msg->otherint;
  fgElement* hold;
  assert(self != 0);
  assert(msg != 0);

  switch(msg->type)
  {
  case FG_CONSTRUCT:
    return FG_ACCEPT;
  case FG_MOVE:
    if(!msg->other && self->parent != 0) // This is internal, so we must always propagate it up
      _sendsubmsg<FG_MOVE, void*, size_t>(self->parent, msg->subtype, self, msg->otheraux | FGMOVE_PROPAGATE);
    if((msg->otheraux & FGMOVE_PROPAGATE) != 0 && !(((fgElement*)msg->other)->flags&FGELEMENT_BACKGROUND)) // A child moved, so recalculate any layouts
      _sendsubmsg<FG_LAYOUTCHANGE, void*, size_t>(self, FGELEMENT_LAYOUTMOVE, msg->other, msg->otheraux);
    else if(msg->otheraux) // This was either internal or propagated down, in which case we must keep propagating it down so long as something changed.
    {
      if(msg->otheraux & (FGMOVE_RESIZE | FGMOVE_PADDING | FGMOVE_MARGIN)) // a layout change can happen on a resize or padding change
        _sendsubmsg<FG_LAYOUTCHANGE, void*, size_t>(self, FGELEMENT_LAYOUTRESIZE, 0, msg->otheraux);

      fgElement* ref = !msg->other ? self : (fgElement*)msg->other;
      fgElement* cur = self->root;
      char diff;
      while(hold = cur)
      {
        cur = cur->next;
        diff = fgElement_PotentialResize(hold);

        //if(diff & msg->otheraux)
        _sendsubmsg<FG_MOVE, void*, size_t>(hold, msg->subtype, ref, diff & msg->otheraux);
      }
    }
    return FG_ACCEPT;
  case FG_SETALPHA:
    return 0;
  case FG_SETAREA:
    if(!msg->other)
      return 0;
    {
      CRect* area = (CRect*)msg->other;

      char diff = CompareCRects(&self->transform.area, area);
      if(diff)
      {
        fgElement_MouseMoveCheck(self);
        fgroot_instance->backend.fgDirtyElement(self);
        memcpy(&self->transform.area, area, sizeof(CRect));
        if(msg->subtype != 0 && msg->subtype != (unsigned short)-1)
          fgResolveCRectUnit(self, self->transform.area, msg->subtype);
        fgroot_instance->backend.fgDirtyElement(self);
        fgElement_MouseMoveCheck(self);

        _sendsubmsg<FG_MOVE, void*, size_t>(self, FG_SETAREA, 0, diff);
      }
      return diff;
    }
  case FG_SETTRANSFORM:
    if(!msg->other)
      return 0;
    {
      fgTransform* transform = (fgTransform*)msg->other;
      _sendmsg<FG_SETAREA, void*>(self, &transform->area);
      char diff = CompareTransforms(&self->transform, transform) & ((1 << 5)|(1 << 6)|(1 << 7));
      if(diff)
      {
        fgElement_MouseMoveCheck(self);
        fgroot_instance->backend.fgDirtyElement(self);
        self->transform.center = transform->center;
        if(msg->subtype != 0 && msg->subtype != (unsigned short)-1)
          fgResolveCVecUnit(self, self->transform.center, msg->subtype);
        self->transform.rotation = transform->rotation;
        fgroot_instance->backend.fgDirtyElement(self);
        fgElement_MouseMoveCheck(self);

        _sendsubmsg<FG_MOVE, void*, size_t>(self, FG_SETTRANSFORM, 0, diff);
      }
    }
    return FG_ACCEPT;
  case FG_SETFLAG: // If 0 is sent in, disable the flag, otherwise enable. Our internal flag is 1 if clipping disabled, 0 otherwise.
    otherint = T_SETBIT(self->flags, otherint, msg->otheraux);
  case FG_SETFLAGS:
  {
    fgFlag change = self->flags ^ (fgFlag)otherint;
    if(change&FGELEMENT_BACKGROUND && !(self->flags & FGELEMENT_BACKGROUND) && self->parent != 0) // if we added the background flag, remove this from the layout before setting the flags.
      _sendsubmsg<FG_LAYOUTCHANGE, void*, size_t>(self->parent, FGELEMENT_LAYOUTREMOVE, self, 0);

    if(change&FGELEMENT_IGNORE || change&FGELEMENT_NOCLIP)
    {
      fgElement* old = self->next;
      if(self->parent != 0) LList_RemoveAll(self);
      self->flags = (fgFlag)otherint;
      if(self->parent != 0) LList_InsertAll(self, old);
    }
    else
      self->flags = (fgFlag)otherint;

    if(change&FGELEMENT_BACKGROUND && !(self->flags & FGELEMENT_BACKGROUND) && self->parent != 0) // If we removed the background flag, add this into the layout after setting the new flags.
      _sendsubmsg<FG_LAYOUTCHANGE, void*, size_t>(self->parent, FGELEMENT_LAYOUTADD, self, 0);
    if((change&FGELEMENT_EXPAND)&self->flags) // If we change the expansion flags, we must recalculate every single child in our layout provided one of the expansion flags is actually set
    {
      if(change&FGELEMENT_EXPANDX) self->layoutdim.x = 0;
      if(change&FGELEMENT_EXPANDY) self->layoutdim.y = 0;
      _sendsubmsg<FG_LAYOUTCHANGE, void*, size_t>(self, FGELEMENT_LAYOUTRESET, 0, 0);
    }
    if(change&FGELEMENT_HIDDEN || change&FGELEMENT_NOCLIP)
      fgroot_instance->backend.fgDirtyElement(self);
  }
  return FG_ACCEPT;
  case FG_SETMARGIN:
    if(!msg->other)
      return 0;
    {
      AbsRect* margin = (AbsRect*)msg->other;
      char diff = CompareMargins(&self->margin, margin);

      if(diff)
      {
        fgElement_MouseMoveCheck(self);
        fgroot_instance->backend.fgDirtyElement(self);
        memcpy(&self->margin, margin, sizeof(AbsRect));
        if(msg->subtype != 0 && msg->subtype != (unsigned short)-1)
          fgResolveRectUnit(self, self->margin, msg->subtype);
        fgroot_instance->backend.fgDirtyElement(self);
        fgElement_MouseMoveCheck(self);

        _sendsubmsg<FG_MOVE, void*, size_t>(self, FG_SETMARGIN, 0, diff | FGMOVE_MARGIN);
      }
    }
    return FG_ACCEPT;
  case FG_SETPADDING:
    if(!msg->other)
      return 0;
    {
      AbsRect* padding = (AbsRect*)msg->other;
      char diff = CompareMargins(&self->padding, padding);

      if(diff)
      {
        fgElement_MouseMoveCheck(self);
        fgroot_instance->backend.fgDirtyElement(self);
        memcpy(&self->padding, padding, sizeof(AbsRect));
        if(msg->subtype != 0 && msg->subtype != (unsigned short)-1)
          fgResolveRectUnit(self, self->padding, msg->subtype);
        fgroot_instance->backend.fgDirtyElement(self);
        fgElement_MouseMoveCheck(self);

        _sendsubmsg<FG_MOVE, void*, size_t>(self, FG_SETPADDING, 0, diff | FGMOVE_PADDING);
      }
    }
    return FG_ACCEPT;
  case FG_SETPARENT: // Note: Doing everything in SETPARENT is a bad idea because it prevents parents from responding to children being added or removed!
  {
    fgElement* parent = (fgElement*)msg->other;
    fgElement* next = (fgElement*)msg->other2;
    if(self->parent == parent)
    {
      if(self->next != next)
      {
        assert(self->parent != 0);
        assert(!next || next->parent == self->parent);
        fgElement* old = self->next;
        LList_RemoveAll(self);
        LList_InsertAll(self, next);
        if(!(self->flags&FGELEMENT_BACKGROUND))
          _sendsubmsg<FG_LAYOUTCHANGE, void*, size_t>(self->parent, FGELEMENT_LAYOUTREORDER, self, (ptrdiff_t)old);
      }
      return FG_ACCEPT;
    }
    if(self->parent != 0)
      _sendmsg<FG_REMOVECHILD, void*>(self->parent, self);
    if(parent)
      _sendmsg<FG_ADDCHILD, void*, void*>(parent, self, next);
  }
    return FG_ACCEPT;
  case FG_ADDITEM:
    if(msg->subtype != 0)
      return 0;
  case FG_ADDCHILD:
    hold = (fgElement*)msg->other;
    if(!hold)
      return 0;
    if(hold->parent != 0) // If the parent is nonzero, call SETPARENT to clean things up for us and then call this again after the child is ready.
      return _sendmsg<FG_SETPARENT, void*, void*>(hold, self, msg->other2); // We do things this way so parents can respond to children being added or removed
    assert(!hold->parent);
    assert(msg->other2 != self);
    hold->parent = self;
    _sendmsg<FG_SETSKIN>(hold);
    LList_InsertAll(hold, (fgElement*)msg->other2);
    if(!(hold->flags&FGELEMENT_BACKGROUND))
      _sendsubmsg<FG_LAYOUTCHANGE, void*, size_t>(self, FGELEMENT_LAYOUTADD, hold, 0);

    assert(!hold->last || !hold->last->next);
    assert(!hold->lastinject || !hold->lastinject->nextinject);
    assert(!hold->lastnoclip || !hold->lastnoclip->nextnoclip);
    _sendsubmsg<FG_MOVE, void*, size_t>(hold, FG_SETPARENT, 0, fgElement_PotentialResize(self));
    return FG_ACCEPT;
  case FG_REMOVECHILD:
    hold = (fgElement*)msg->other;
    if(!msg->other || hold->parent != self)
      return 0;

    if(!(hold->flags&FGELEMENT_BACKGROUND))
      _sendsubmsg<FG_LAYOUTCHANGE, void*, size_t>(self, FGELEMENT_LAYOUTREMOVE, hold, 0);
    if(self->lastfocus == hold)
      self->lastfocus = 0;
    LList_RemoveAll(hold); // Remove hold from us
    
    if(self->skinelements != 0) // Double check to see if this is a skin element. If so, remove it from our hash
    {
      khiter_t iter = kh_get_fgSkinElements(self->skinelements, hold);
      if(iter != kh_end(self->skinelements) && kh_exist(self->skinelements, iter))
        kh_del_fgSkinElements(self->skinelements, iter);
    }
    hold->parent = 0;
    hold->next = 0;
    hold->prev = 0;
    _sendmsg<FG_SETSKIN>(hold);
    _sendsubmsg<FG_MOVE, void*, size_t>(hold, FG_SETPARENT, 0, fgElement_PotentialResize(self));
    return FG_ACCEPT;
  case FG_LAYOUTCHANGE:
    assert(!msg->other || !(((fgElement*)msg->other)->flags&FGELEMENT_BACKGROUND));
    if(self->flags&FGELEMENT_EXPAND)
    {
      AbsVec newdim = self->layoutdim;

      _sendmsg<FG_LAYOUTFUNCTION, const void*, void*>(self, msg, &newdim);
      assert(!isnan(newdim.x) && !isnan(newdim.y));
      char diff = 0;
      if((self->flags&FGELEMENT_EXPANDX) && newdim.x != self->layoutdim.x) diff |= FGMOVE_RESIZEX;
      if((self->flags&FGELEMENT_EXPANDY) && newdim.y != self->layoutdim.y) diff |= FGMOVE_RESIZEY;

      if(diff)
      {
        fgElement_MouseMoveCheck(self);
        fgroot_instance->backend.fgDirtyElement(self);
        self->layoutdim = newdim;
        fgroot_instance->backend.fgDirtyElement(self);
        fgElement_MouseMoveCheck(self);
        _sendsubmsg<FG_MOVE, void*, size_t>(self, FG_LAYOUTCHANGE, 0, diff);
      }
    }

    return FG_ACCEPT;
  case FG_LAYOUTFUNCTION:
    return ((self->flags & FGELEMENT_EXPAND) || msg->subtype != 0) ? fgDefaultLayout(self, (const FG_Msg*)msg->other, (AbsVec*)msg->other2) : 0;
  case FG_LAYOUTLOAD:
  {
    fgLayout* layout = (fgLayout*)msg->other;
    if(!layout)
      return 0;

    _sendsubmsg<FG_SETSTYLE, void*, size_t>(self, FGSETSTYLE_POINTER, &layout->style, ~0);
    for(FG_UINT i = 0; i < layout->layout.l; ++i)
      fgElement_LoadLayout(self, 0, layout->layout.p + i);
  }
  return FG_ACCEPT;
  /*case FG_CLONE: // REMOVED: Cloning is extraordinarily difficult to get right due to skinref, which needs a minimum O(n^2) operation to be cloned correctly.
  {
    hold = (fgElement*)msg->other;
    if(!hold)
    {
      hold = fgmalloc<fgElement>(1, __FILE__, __LINE__);
      memcpy(hold, self, sizeof(fgElement));
#ifdef BSS_DEBUG
      hold->free = &fgfreeblank;
#else
      hold->free = &free;
#endif
    }
    else
      memcpy(hold, self, sizeof(fgElement));

    hold->root = 0;
    hold->last = 0;
    hold->parent = 0;
    hold->next = 0;
    hold->prev = 0;
    hold->nextinject = 0;
    hold->previnject = 0;
    hold->nextnoclip = 0;
    hold->prevnoclip = 0;
    hold->rootnoclip = 0;
    hold->lastnoclip = 0;
    hold->rootinject = 0;
    hold->lastinject = 0;
    hold->name = fgCopyText(self->name, __FILE__, __LINE__);
    hold->userhash = 0;
    hold->skinrefs.l = 0;
    hold->SetParent(self->parent, self->next);

    fgElement* cur = self->root;
    while(cur)
    {
      _sendmsg<FG_ADDCHILD, void*>(hold, (fgElement*)_sendmsg<FG_CLONE>(cur));
      cur = cur->next;
    }

    if(!msg->other)
      return (size_t)hold;
  }
  return 0;*/
  case FG_GETCLASSNAME:
    return (size_t)"Element";
  case FG_GETSKIN:
    if(!msg->other) // If msg->other is none we simply return our self->skin, whatever it is
      return reinterpret_cast<size_t>(self->skin);
    if(self->skin != 0) // Otherwise we are performing a skin lookup for a child
    {
      hold = (fgElement*)msg->other;
      const char* name = hold->GetName();
      if(name)
      {
        fgSkin* skin = fgSkin_GetSkin(self->skin, name);
        if(skin != 0)
          return reinterpret_cast<size_t>(skin);
      }
      name = hold->GetClassName();

      fgSkin* skin = fgSkin_GetSkin(self->skin, name);
      if(skin != 0)
        return reinterpret_cast<size_t>(skin);
    }
    return (!self->parent) ? 0 : fgPassMessage(self->parent, msg);
  case FG_SETSKIN:
  {
    fgSkin* skin = (fgSkin*)msg->other;
    if(!skin && self->parent != 0)
      skin = (fgSkin*)_sendmsg<FG_GETSKIN, void*>(self->parent, self);
    if(self->skin != skin) // only bother changing the skin if there's stuff to change
    {
      if(self->skin != 0 && self->skinelements != 0) // remove existing skin elements
      {
        for(FG_UINT i = 0; i < self->skinelements->n_buckets; ++i)
          if(kh_exist(self->skinelements, i))
            VirtualFreeChild(kh_key(self->skinelements, i));
        kh_clear_fgSkinElements(self->skinelements);
      }
      self->skin = skin;
      if(self->skin != 0)
        fgElement_ApplySkin(self, self->skin);
    }

    fgElement* cur = self->root;
    while(cur) // Because we allow children to look up their entire inheritance tree we must always inform them of skin changes
    {
      cur->SetSkin(0);
      cur = cur->next;
    }
    _sendsubmsg<FG_SETSTYLE, ptrdiff_t, size_t>(self, FGSETSTYLE_INDEX, -1, ~0); // force us and our children to recalculate our style based on the new skin
  }
  return FG_ACCEPT;
  case FG_SETSTYLE:
  {
    fgStyle* style = 0;
    if(msg->subtype != FGSETSTYLE_POINTER)
    {
      assert(msg->otheraux != 0);
      FG_UINT mask = (FG_UINT)msg->otheraux;
      FG_UINT index;

      switch(msg->subtype)
      {
      case FGSETSTYLE_NAME:
      case FGSETSTYLE_INDEX:
        index = (!msg->subtype ? fgStyle_GetName((const char*)msg->other, false) : (FG_UINT)msg->otherint);

        if(index == (FG_UINT)-1)
          index = (FG_UINT)_sendmsg<FG_GETSTYLE>(self);
        else if(self->style == (FG_UINT)-1)
          self->style = index;
        else
          self->style = (FG_UINT)(index | (self->style&(~mask)));
        break;
      case FGSETSTYLE_SETFLAG:
      case FGSETSTYLE_REMOVEFLAG:
        index = fgStyle_GetName((const char*)msg->other, true);
      case FGSETSTYLE_SETFLAGINDEX:
      case FGSETSTYLE_REMOVEFLAGINDEX:
        if(msg->subtype == FGSETSTYLE_SETFLAGINDEX || msg->subtype == FGSETSTYLE_REMOVEFLAGINDEX)
          index = (FG_UINT)msg->otherint;
        if(self->style == (FG_UINT)-1)
          self->style = 0;
        self->style = T_SETBIT(self->style, index, (msg->subtype == FGSETSTYLE_SETFLAG));
        index = (self->style&mask);
        break;
      }

      fgElement* cur = self->root;
      while(cur)
      {
        _sendsubmsg<FG_SETSTYLE, ptrdiff_t, size_t>(cur, FGSETSTYLE_INDEX, -1, ~0); // Forces the child to recalculate the style inheritance
        cur = cur->next;
      }

      FG_Msg m = *msg;
      m.subtype = FGSETSTYLE_POINTER;
      FG_UINT flags = ((self->style == (FG_UINT)-1) ? (index&fgStyleFlagMask) : (self->style&fgStyleFlagMask));
      index &= (~fgStyleFlagMask);
      while(index) // We loop through all set bits of index according to mask. index is not always just a single flag, because of style resets when the mask is -1.
      {
        FG_UINT indice = (1U << bss_util::bsslog2(index));
        index ^= indice;
        if(self->skin != 0)
        {
          m.other = fgSkin_GetStyle(self->skin, indice | flags);
          if(m.other != 0)
            fgElement_Message(self, &m);
          else if(flags != 0) // If we failed to find a style, but some flags were set, disable the flags and try to find another one
          {
            m.other = fgSkin_GetStyle(self->skin, indice);
            if(m.other != 0)
              fgElement_Message(self, &m);
          }
        }
      }
    }
    else
      style = (fgStyle*)msg->other;

    if(!style)
      return 0;

    fgStyleMsg* cur = style->styles;
    while(cur)
    {
      (*fgroot_instance->backend.behaviorhook)(self, &cur->msg);
      cur = cur->next;
    }
  }
  return FG_ACCEPT;
  case FG_GETSTYLE:
    return (self->style == (FG_UINT)-1 && self->parent != 0) ? (*fgroot_instance->backend.behaviorhook)(self->parent, msg) : self->style;
  case FG_GOTFOCUS:
    if(self->parent)
      return (*fgroot_instance->backend.behaviorhook)(self->parent, msg);
    break;
  case FG_DRAGOVER:
    fgRoot_SetCursor(FGCURSOR_NO, 0);
    return 0;
  case FG_DROP:
    return 0;
  case FG_DRAW:
    fgStandardDraw(self, (const AbsRect*)msg->other, (const fgDrawAuxData*)msg->other2, msg->subtype & 1);
    return FG_ACCEPT;
  case FG_INJECT:
    return fgStandardInject(self, (const FG_Msg*)msg->other, (const AbsRect*)msg->other2);
  case FG_SETNAME:
    if(self->name) fgfree(self->name, __FILE__, __LINE__);
    self->name = fgCopyText((const char*)msg->other, __FILE__, __LINE__);
    _sendmsg<FG_SETSKIN, void*>(self, 0); // force the skin to be recalculated
    return FG_ACCEPT;
  case FG_GETNAME:
    return (size_t)self->name;
  case FG_GETDPI:
    return self->parent ? (*fgroot_instance->backend.behaviorhook)(self->parent, msg) : (size_t)&fgIntVec_EMPTY;
  case FG_GETLINEHEIGHT:
    return self->parent ? (*fgroot_instance->backend.behaviorhook)(self->parent, msg) : 0;
  case FG_SETDPI:
  {
    fgElement* cur = self->root;
    while(hold = cur)
    {
      cur = cur->next;
      fgPassMessage(hold, msg);
    }
  }
  break;
  case FG_TOUCHBEGIN:
    fgroot_instance->mouse.buttons |= FG_MOUSELBUTTON;
    self->MouseDown(msg->x, msg->y, FG_MOUSELBUTTON, fgroot_instance->mouse.buttons);
    break;
  case FG_TOUCHEND:
    fgroot_instance->mouse.buttons &= ~FG_MOUSELBUTTON;
    self->MouseUp(msg->x, msg->y, FG_MOUSELBUTTON, fgroot_instance->mouse.buttons);
    break;
  case FG_TOUCHMOVE:
    fgroot_instance->mouse.buttons |= FG_MOUSELBUTTON;
    self->MouseMove(msg->x, msg->y);
    break;
  case FG_SETDIM:
  {
    char diff = 0;
    switch(msg->subtype)
    {
    case FGDIM_MAX:
      if(self->maxdim.x != msg->otherf) diff |= FGMOVE_RESIZEX;
      if(self->maxdim.y != msg->otherfaux) diff |= FGMOVE_RESIZEY;
      break;
    case FGDIM_MIN:
      if(self->mindim.x != msg->otherf) diff |= FGMOVE_RESIZEX;
      if(self->mindim.y != msg->otherfaux) diff |= FGMOVE_RESIZEY;
      break;
    default:
      return 0;
    }
    if(diff != 0)
    {
      fgElement_MouseMoveCheck(self);
      fgroot_instance->backend.fgDirtyElement(self);
      switch(msg->subtype)
      {
      case FGDIM_MAX:
        self->maxdim.x = msg->otherf;
        self->maxdim.y = msg->otherfaux;
        if((msg->subtype&(FGUNIT_X_MASK | FGUNIT_Y_MASK)) != 0)
          fgResolveVecUnit(self, self->maxdim, msg->subtype);
        break;
      case FGDIM_MIN:
        self->mindim.x = msg->otherf;
        self->mindim.y = msg->otherfaux;
        if((msg->subtype&(FGUNIT_X_MASK | FGUNIT_Y_MASK)) != 0)
          fgResolveVecUnit(self, self->mindim, msg->subtype);
        break;
      }
      fgroot_instance->backend.fgDirtyElement(self);
      fgElement_MouseMoveCheck(self);
      _sendsubmsg<FG_MOVE, void*, size_t>(self, FG_SETDIM, 0, diff);
    }
  }
    return FG_ACCEPT;
  case FG_GETDIM:
    switch(msg->subtype)
    {
    case FGDIM_MAX:
      return *reinterpret_cast<size_t*>(&self->maxdim);
    case FGDIM_MIN:
      return *reinterpret_cast<size_t*>(&self->mindim);
    }
    return 0;
  case FG_GETUSERDATA:
    if(!msg->other2)
      return *reinterpret_cast<size_t*>(&self->userdata);
    if(!self->userhash)
      return 0;
    {
      khiter_t k = kh_get_fgUserdata(self->userhash, (char*)msg->other2);
      return (k != kh_end(self->userhash) && kh_exist(self->userhash, k)) ? kh_val(self->userhash, k) : 0;
    }
  case FG_SETUSERDATA:
    if(!msg->other2)
    {
      self->userdata = msg->other;
      return 1;
    }
    if(!self->userhash)
      self->userhash = kh_init_fgUserdata();
    if(!self->userhash)
      return 0;

    {
      int r = 0;
      const char* name = (const char*)msg->other2;
      khiter_t iter = kh_get_fgUserdata(self->userhash, (char*)msg->other2);
      if(iter == kh_end(self->userhash) || !kh_exist(self->userhash, iter))
        iter = kh_put_fgUserdata(self->userhash, fgCopyText((const char*)msg->other2, __FILE__, __LINE__), &r);
      kh_val(self->userhash, iter) = (size_t)msg->otherint;
      return 1 + r;
    }
  case FG_GETSELECTEDITEM:
    break;
  case FG_MOUSEDBLCLICK:
    return self->MouseDown(msg->x, msg->y, msg->button, msg->allbtn);
  case FG_SETSCALING:
    self->scaling.x = msg->otherf;
    self->scaling.y = msg->otherfaux;
    break;
  case FG_GETSCALING:
    return (size_t)&self->scaling;
  }

  return 0;
}

void FG_FASTCALL VirtualFreeChild(fgElement* self)
{
  assert(self != 0);
  (*self->destroy)(self);
  if(self->free)
    (*self->free)(self);
}

BSS_FORCEINLINE void __applyrect(AbsRect& dest, const AbsRect& src, const AbsRect& apply) noexcept
{
  const sseVecT<FABS> m(1.0f, 1.0f, -1.0f, -1.0f);
  (sseVecT<FABS>(BSS_UNALIGNED<const float>(&src.left)) + (sseVecT<FABS>(BSS_UNALIGNED<const float>(&apply.left))*m)) >> BSS_UNALIGNED<float>(&dest.left);
}

// Inner (Child) rect has padding and margins applied and is used by foreground elements
// Standard (clipping) rect has margins applied is used by background elements and rendering
// Outer (layout) rect has none of those and is used by layouts

void FG_FASTCALL ResolveOuterRect(const fgElement* self, AbsRect* out)
{
  assert(out != 0);
  if(!self->parent)
  {
    const CRect& v = self->transform.area;
    out->left = v.left.abs;
    out->top = v.top.abs;
    out->right = v.right.abs;
    out->bottom = v.bottom.abs;
    return;
  }

  AbsRect last;
  ResolveRect(self->parent, &last);
  ResolveOuterRectCache(self, out, &last, (self->flags & FGELEMENT_BACKGROUND) ? 0 : &self->parent->padding);
}

void FG_FASTCALL ResolveOuterRectCache(const fgElement* self, AbsRect* BSS_RESTRICT out, const AbsRect* BSS_RESTRICT last, const AbsRect* BSS_RESTRICT padding)
{
  AbsRect replace;
  if(padding != 0)
  {
    __applyrect(replace, *last, *padding);
    last = &replace;
  }

  AbsVec center = { self->transform.center.x.abs, self->transform.center.y.abs };
  const CRect* v = &self->transform.area;
  assert(out != 0 && self != 0 && last != 0);
  //bss_util::lerp<sseVecT<FABS>, sseVecT<FABS>>(
  //  sseVecT<FABS>(last->left, last->top, last->left, last->top),
  //  sseVecT<FABS>(last->right, last->bottom, last->right, last->bottom),
  //  sseVecT<FABS>(v->left.rel, v->top.rel, v->right.rel, v->bottom.rel))
  //  + sseVecT<FABS>(v->left.abs, v->top.abs, v->right.abs, v->bottom.abs) >> BSS_UNALIGNED<float>(&out->left);
  out->left = fglerp(last->left, last->right, v->left.rel) + v->left.abs;
  out->top = fglerp(last->top, last->bottom, v->top.rel) + v->top.abs;
  out->right = fglerp(last->left, last->right, v->right.rel) + v->right.abs;
  out->bottom = fglerp(last->top, last->bottom, v->bottom.rel) + v->bottom.abs;

  if(self->flags & FGELEMENT_EXPANDX)
    out->right = out->left + std::max(out->right - out->left, self->layoutdim.x + self->padding.left + self->padding.right + self->margin.left + self->margin.right);
  if(self->flags & FGELEMENT_EXPANDY)
    out->bottom = out->top + std::max(out->bottom - out->top, self->layoutdim.y + self->padding.top + self->padding.bottom + self->margin.top + self->margin.bottom);

  if(self->mindim.x >= 0)
    out->right = out->left + std::max(out->right - out->left, self->mindim.x);
  if(self->mindim.y >= 0)
    out->bottom = out->top + std::max(out->bottom - out->top, self->mindim.y);
  if(self->maxdim.x >= 0)
    out->right = out->left + std::min(out->right - out->left, self->maxdim.x);
  if(self->maxdim.y >= 0)
    out->bottom = out->top + std::min(out->bottom - out->top, self->maxdim.y);

  center.x += (out->right - out->left)*self->transform.center.x.rel;
  center.y += (out->bottom - out->top)*self->transform.center.y.rel;
  out->left -= center.x;
  out->top -= center.y;
  out->right -= center.x;
  out->bottom -= center.y;

  if(self->flags&FGELEMENT_SNAPX) { // TODO: Make this work properly with DPI so it maps to pixels
    out->left = floor(out->left);
    out->right = floor(out->right);
  }
  if(self->flags&FGELEMENT_SNAPY) {
    out->top = floor(out->top);
    out->bottom = floor(out->bottom);
  }
  assert(!isnan(out->left) && !isnan(out->top) && !isnan(out->right) && !isnan(out->bottom));
}

void FG_FASTCALL ResolveRect(const fgElement* self, AbsRect* out)
{
  ResolveOuterRect(self, out);
  __applyrect(*out, *out, self->margin);
}

void FG_FASTCALL ResolveRectCache(const fgElement* self, AbsRect* BSS_RESTRICT out, const AbsRect* BSS_RESTRICT last, const AbsRect* BSS_RESTRICT padding)
{
  ResolveOuterRectCache(self, out, last, padding);
  __applyrect(*out, *out, self->margin);
}

void FG_FASTCALL ResolveInnerRect(const fgElement* self, AbsRect* out)
{
  ResolveRect(self, out);
  __applyrect(*out, *out, self->padding);
}

void FG_FASTCALL GetInnerRect(const fgElement* self, AbsRect* inner, const AbsRect* standard)
{
  __applyrect(*inner, *standard, self->padding);
}

char FG_FASTCALL MsgHitElement(const FG_Msg* msg, const fgElement* child)
{
  AbsRect r;
  assert(msg != 0 && child != 0);
  ResolveRect(child, &r);
  return MsgHitAbsRect(msg, &r);
}

size_t FG_FASTCALL fgVoidMessage(fgElement* self, unsigned short type, void* data, ptrdiff_t aux)
{
  FG_Msg msg = { 0 };
  msg.type = type;
  msg.other = data;
  msg.otheraux = aux;
  assert(self != 0);
  return (*fgroot_instance->backend.behaviorhook)(self, &msg);
}

size_t FG_FASTCALL fgIntMessage(fgElement* self, unsigned short type, ptrdiff_t data, size_t aux)
{
  FG_Msg msg = { 0 };
  msg.type = type;
  msg.otherint = data;
  msg.otheraux = aux;
  assert(self != 0);
  return (*fgroot_instance->backend.behaviorhook)(self, &msg);
}

size_t FG_FASTCALL fgPassMessage(fgElement* self, const FG_Msg* msg)
{
  return (*fgroot_instance->backend.behaviorhook)(self, msg);
}

size_t FG_FASTCALL fgSubMessage(fgElement* self, unsigned short type, unsigned short subtype, void* data, ptrdiff_t aux)
{
  FG_Msg msg = { 0 };
  msg.type = type;
  msg.subtype = subtype;
  msg.other = data;
  msg.otheraux = aux;
  assert(self != 0);
  return (*fgroot_instance->backend.behaviorhook)(self, &msg);
}

void FG_FASTCALL fgElement_Clear(fgElement* self)
{
  while(self->root) // Destroy all children
    VirtualFreeChild(self->root);
}

fgElement* FG_FASTCALL fgElement_GetChildUnderMouse(fgElement* self, int x, int y, AbsRect* cache)
{
  ResolveRect(self, cache);
  AbsRect child;
  size_t l = self->GetNumItems(); 
  if(l > 0) // If this is a control inheriting fgBox we can use a binary search instead
  {
    fgElement* cur = self->GetItemAt(x, y);
    if(cur != 0 && !(cur->flags&FGELEMENT_BACKGROUND))
    {
      ResolveRectCache(cur, &child, cache, &self->padding);
      if(HitAbsRect(&child, (FABS)x, (FABS)y))
        return cur;
    }
    return 0;
  }

  fgElement* cur = self->root; // we have to go through the whole child list because you can have FG_IGNORE on but still be a hover target, espiecally if you're an fgText control inside a list.
  while(cur != 0)
  {
    if(!(cur->flags&FGELEMENT_BACKGROUND))
    {
      ResolveRectCache(cur, &child, cache, &self->padding); // this is only done for non background elements, so we always pass in the padding.
      if(HitAbsRect(&child, (FABS)x, (FABS)y))
        return cur;
    }
    cur = cur->next;
  }
  return 0;
}

void FG_FASTCALL fgElement_MouseMoveCheck(fgElement* self)
{
  if(!self->parent || (self->flags&FGELEMENT_IGNORE))
    return; // If you have no parent or FGELEMENT_IGNORE is set, you can't possibly recieve MOUSEMOVE events so don't bother with this.
  AbsRect out;
  ResolveRect(self, &out);
  if(HitAbsRect(&out, (FABS)fgroot_instance->mouse.x, (FABS)fgroot_instance->mouse.y))
    fgroot_instance->mouse.state |= FGMOUSE_SEND_MOUSEMOVE;
}

void FG_FASTCALL fgElement_AddListener(fgElement* self, unsigned short type, fgListener listener)
{
  fgListenerList.Insert(std::pair<fgElement*, unsigned short>(self, type));
  fgListenerHash.Insert(std::pair<fgElement*, unsigned short>(self, type), listener);
}

bss_util::AVL_Node<std::pair<fgElement*, unsigned short>>* FG_FASTCALL fgElement_GetAnyListener(fgElement* key, bss_util::AVL_Node<std::pair<fgElement*, unsigned short>>* cur)
{
  while(cur)
  {
    switch(SGNCOMPARE(cur->_key.first, key)) // by only comparing the first half of the key, we can find all the nodes for a given fgElement*
    {
    case -1: cur = cur->_left; break;
    case 1: cur = cur->_right; break;
    default: return cur;
    }
  }

  return 0;
}
void FG_FASTCALL fgElement_ClearListeners(fgElement* self)
{
  bss_util::AVL_Node<std::pair<fgElement*, unsigned short>>* cur;

  while(cur = fgElement_GetAnyListener(self, fgListenerList.GetRoot()))
  {
    fgListenerHash.Remove(cur->_key);
    fgListenerList.Remove(cur->_key);
  }
} 

void fgElement::Construct() { _sendmsg<FG_CONSTRUCT>(this); }

void FG_FASTCALL fgElement::Move(unsigned short subtype, fgElement* child, size_t diff) { _sendsubmsg<FG_MOVE, void*, size_t>(this, subtype, child, diff); }

size_t FG_FASTCALL fgElement::SetAlpha(float alpha) { return _sendmsg<FG_SETALPHA, float>(this, alpha); }

size_t FG_FASTCALL fgElement::SetArea(const CRect& area) { return _sendmsg<FG_SETAREA, const void*>(this, &area); }

size_t FG_FASTCALL fgElement::SetTransform(const fgTransform& transform) { return _sendmsg<FG_SETTRANSFORM, const void*>(this, &transform); }

void FG_FASTCALL fgElement::SetFlag(fgFlag flag, bool value) { _sendmsg<FG_SETFLAG, ptrdiff_t, size_t>(this, flag, value != 0); }

void FG_FASTCALL fgElement::SetFlags(fgFlag flags) { _sendmsg<FG_SETFLAGS, ptrdiff_t>(this, flags); }

size_t FG_FASTCALL fgElement::SetMargin(const AbsRect& margin) { return _sendmsg<FG_SETMARGIN, const void*>(this, &margin); }

size_t FG_FASTCALL fgElement::SetPadding(const AbsRect& padding) { return _sendmsg<FG_SETPADDING, const void*>(this, &padding); }

void FG_FASTCALL fgElement::SetParent(fgElement* parent, fgElement* next) { _sendmsg<FG_SETPARENT, void*, void*>(this, parent, next); }

size_t FG_FASTCALL fgElement::AddChild(fgElement* child, fgElement* next) { return _sendmsg<FG_ADDCHILD, void*, void*>(this, child, next); }

fgElement* FG_FASTCALL fgElement::AddItem(void* item, size_t index) { return (fgElement*)_sendsubmsg<FG_ADDITEM, const void*, size_t>(this, FGITEM_DEFAULT, item, index); }
fgElement* FG_FASTCALL fgElement::AddItemText(const char* item, FGSETTEXT fmt) { return (fgElement*)_sendsubmsg<FG_ADDITEM, const void*, size_t>(this, FGITEM_TEXT, item, fmt); }
fgElement* FG_FASTCALL fgElement::AddItemElement(fgElement* item, size_t index) { return (fgElement*)_sendsubmsg<FG_ADDITEM, const void*, size_t>(this, FGITEM_ELEMENT, item, index); }

size_t FG_FASTCALL fgElement::RemoveChild(fgElement* child) { return _sendmsg<FG_REMOVECHILD, void*>(this, child); }

size_t FG_FASTCALL fgElement::RemoveItem(size_t item) { return _sendmsg<FG_REMOVEITEM, ptrdiff_t>(this, item); }

size_t FG_FASTCALL fgElement::LayoutFunction(const FG_Msg& msg, const CRect& area, bool scrollbar) { return _sendsubmsg<FG_LAYOUTFUNCTION, const void*, const void*>(this, !!scrollbar, &msg, &area); }

void fgElement::LayoutChange(unsigned short subtype, fgElement* target, fgElement* old) { _sendsubmsg<FG_LAYOUTCHANGE, void*, void*>(this, subtype, target, old); }

size_t FG_FASTCALL fgElement::LayoutLoad(fgLayout* layout) { return _sendmsg<FG_LAYOUTLOAD, void*>(this, layout); }

size_t fgElement::DragOver(int x, int y)
{
  FG_Msg m = { 0 };
  m.type = FG_DRAGOVER;
  m.x = x;
  m.y = y;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t fgElement::Drop(int x, int y, unsigned char allbtn)
{
  FG_Msg m = { 0 };
  m.type = FG_DROP;
  m.x = x;
  m.y = y;
  m.allbtn = allbtn;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

void fgElement::Draw(const AbsRect* area, const fgDrawAuxData* aux) { _sendmsg<FG_DRAW, const void*, const void*>(this, area, aux); }

size_t fgElement::Inject(const FG_Msg* msg, const AbsRect* area) { return _sendmsg<FG_INJECT, const void*, const void*>(this, msg, area); }

size_t FG_FASTCALL fgElement::SetSkin(fgSkin* skin) { return _sendmsg<FG_SETSKIN, void*>(this, skin); }

fgSkin* FG_FASTCALL fgElement::GetSkin(fgElement* child) { return reinterpret_cast<fgSkin*>(_sendmsg<FG_GETSKIN, void*>(this, child)); }

size_t FG_FASTCALL fgElement::SetStyle(const char* name, FG_UINT mask) {  return _sendsubmsg<FG_SETSTYLE, const void*, size_t>(this, FGSETSTYLE_NAME, name, mask); }

size_t FG_FASTCALL fgElement::SetStyle(struct _FG_STYLE* style) { return _sendsubmsg<FG_SETSTYLE, void*, size_t>(this, FGSETSTYLE_POINTER, style, ~0); }

size_t FG_FASTCALL fgElement::SetStyle(FG_UINT index, FG_UINT mask) { return _sendsubmsg<FG_SETSTYLE, ptrdiff_t, size_t>(this, FGSETSTYLE_INDEX, index, mask); }

struct _FG_STYLE* fgElement::GetStyle() { return reinterpret_cast<struct _FG_STYLE*>(_sendmsg<FG_GETSTYLE>(this)); }

fgIntVec& FG_FASTCALL fgElement::GetDPI() { return *reinterpret_cast<fgIntVec*>(_sendmsg<FG_GETDPI>(this)); }

void FG_FASTCALL fgElement::SetDPI(int x, int y) { _sendmsg<FG_SETDPI, ptrdiff_t, size_t>(this, x, y); }

const char* fgElement::GetClassName() { return reinterpret_cast<const char*>(_sendmsg<FG_GETCLASSNAME>(this)); }

void* FG_FASTCALL fgElement::GetUserdata(const char* name) { size_t r = _sendmsg<FG_GETUSERDATA, const void*>(this, name); return *reinterpret_cast<void**>(&r); }

void FG_FASTCALL fgElement::SetUserdata(void* data, const char* name) { _sendmsg<FG_SETUSERDATA, void*, const void*>(this, data, name); }

size_t FG_FASTCALL fgElement::MouseDown(int x, int y, unsigned char button, unsigned char allbtn)
{
  FG_Msg m = { 0 };
  m.type = FG_MOUSEDOWN;
  m.x = x;
  m.y = y;
  m.button = button;
  m.allbtn = allbtn;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::MouseDblClick(int x, int y, unsigned char button, unsigned char allbtn)
{
  FG_Msg m = { 0 };
  m.type = FG_MOUSEDBLCLICK;
  m.x = x;
  m.y = y;
  m.button = button;
  m.allbtn = allbtn;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::MouseUp(int x, int y, unsigned char button, unsigned char allbtn)
{
  FG_Msg m = { 0 };
  m.type = FG_MOUSEUP;
  m.x = x;
  m.y = y;
  m.button = button;
  m.allbtn = allbtn;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::MouseOn(int x, int y)
{
  FG_Msg m = { 0 };
  m.type = FG_MOUSEON;
  m.x = x;
  m.y = y;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::MouseOff(int x, int y)
{
  FG_Msg m = { 0 };
  m.type = FG_MOUSEOFF;
  m.x = x;
  m.y = y;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::MouseMove(int x, int y)
{
  FG_Msg m = { 0 };
  m.type = FG_MOUSEMOVE;
  m.x = x;
  m.y = y;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::MouseScroll(int x, int y, unsigned short delta, unsigned short hdelta)
{
  FG_Msg m = { 0 };
  m.type = FG_MOUSESCROLL;
  m.x = x;
  m.y = y;
  m.scrolldelta = delta;
  m.scrollhdelta = hdelta;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::KeyUp(unsigned char keycode, char sigkeys)
{
  FG_Msg m = { 0 };
  m.type = FG_KEYUP;
  m.keycode = keycode;
  m.sigkeys = sigkeys;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::KeyDown(unsigned char keycode, char sigkeys)
{
  FG_Msg m = { 0 };
  m.type = FG_KEYDOWN;
  m.keycode = keycode;
  m.sigkeys = sigkeys;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::KeyChar(int keychar, char sigkeys)
{
  FG_Msg m = { 0 };
  m.type = FG_KEYCHAR;
  m.keychar = keychar;
  m.sigkeys = sigkeys;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::JoyButtonDown(short joybutton)
{
  FG_Msg m = { 0 };
  m.type = FG_JOYBUTTONDOWN;
  m.joybutton = joybutton;
  m.joydown = true;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::JoyButtonUp(short joybutton)
{
  FG_Msg m = { 0 };
  m.type = FG_JOYBUTTONUP;
  m.joybutton = joybutton;
  m.joydown = false;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t FG_FASTCALL fgElement::JoyAxis(float joyvalue, short joyaxis)
{
  FG_Msg m = { 0 };
  m.type = FG_JOYAXIS;
  m.joyvalue = joyvalue;
  m.joyaxis = joyaxis;
  return (*fgroot_instance->backend.behaviorhook)(this, &m);
}

size_t fgElement::GotFocus() { return _sendmsg<FG_GOTFOCUS>(this); }

void fgElement::LostFocus() { _sendmsg<FG_LOSTFOCUS>(this); }

size_t FG_FASTCALL fgElement::SetName(const char* name) { return _sendmsg<FG_SETNAME, const void*>(this, name); }

const char* fgElement::GetName() { return reinterpret_cast<const char*>(_sendmsg<FG_GETNAME>(this)); }

void FG_FASTCALL fgElement::SetContextMenu(fgElement* menu) { _sendmsg<FG_SETCONTEXTMENU, void*>(this, menu); }

fgElement* fgElement::GetContextMenu() { return reinterpret_cast<fgElement*>(_sendmsg<FG_GETCONTEXTMENU>(this)); }

void fgElement::Neutral() { _sendmsg<FG_NEUTRAL>(this); }

void fgElement::Hover() { _sendmsg<FG_HOVER>(this); }

void fgElement::Active() { _sendmsg<FG_ACTIVE>(this); }

void fgElement::Action() { _sendmsg<FG_ACTION>(this); }

void FG_FASTCALL fgElement::SetDim(float x, float y, FGDIM type) { _sendsubmsg<FG_SETDIM, float, float>(this, type, x, y); }

const AbsVec* fgElement::GetDim(FGDIM type) { size_t r = _sendsubmsg<FG_GETDIM>(this, type); return *reinterpret_cast<AbsVec**>(&r); }

size_t fgElement::GetValue(ptrdiff_t aux) { return _sendsubmsg<FG_GETVALUE, ptrdiff_t>(this, FGVALUE_INT64, aux); }

float fgElement::GetValueF(ptrdiff_t aux) { size_t r = _sendsubmsg<FG_GETVALUE, ptrdiff_t>(this, FGVALUE_FLOAT, aux); return *reinterpret_cast<float*>(&r); }

void* fgElement::GetValueP(ptrdiff_t aux) { size_t r = _sendsubmsg<FG_GETVALUE, ptrdiff_t>(this, FGVALUE_POINTER, aux); return *reinterpret_cast<void**>(&r); }

size_t fgElement::SetValue(ptrdiff_t state, size_t aux) { return _sendsubmsg<FG_SETVALUE, ptrdiff_t, size_t>(this, FGVALUE_INT64, state, aux); }

size_t fgElement::SetValueF(float state, size_t aux) { return _sendsubmsg<FG_SETVALUE, float, size_t>(this, FGVALUE_FLOAT, state, aux); }

size_t fgElement::SetValueP(void* ptr, size_t aux) { return _sendsubmsg<FG_SETVALUE, void*, size_t>(this, FGVALUE_POINTER, ptr, aux); }

struct _FG_ELEMENT* fgElement::GetItem(ptrdiff_t index) { return reinterpret_cast<fgElement*>(_sendmsg<FG_GETITEM, ptrdiff_t>(this, index)); }

struct _FG_ELEMENT* fgElement::GetItemAt(int x, int y)
{
  FG_Msg m = { 0 };
  m.type = FG_GETITEM;
  m.subtype = FGITEM_LOCATION;
  m.x = x;
  m.y = y;
  return reinterpret_cast<fgElement*>((*fgroot_instance->backend.behaviorhook)(this, &m));
}

size_t fgElement::GetNumItems() { return _sendsubmsg<FG_GETITEM>(this, FGITEM_COUNT); }

fgElement* fgElement::GetSelectedItem(ptrdiff_t index) { return reinterpret_cast<fgElement*>(_sendmsg<FG_GETSELECTEDITEM, ptrdiff_t>(this, index)); }

size_t FG_FASTCALL fgElement::SetResource(void* res) { return _sendmsg<FG_SETRESOURCE, void*>(this, res); }

size_t FG_FASTCALL fgElement::SetUV(const CRect& uv) { return _sendmsg<FG_SETUV, const void*>(this, &uv); }

size_t FG_FASTCALL fgElement::SetColor(unsigned int color, FGSETCOLOR index) { return _sendsubmsg<FG_SETCOLOR, ptrdiff_t>(this, index, color); }

size_t FG_FASTCALL fgElement::SetOutline(float outline) { return _sendmsg<FG_SETOUTLINE, float>(this, outline); }

size_t FG_FASTCALL fgElement::SetFont(void* font) { return _sendmsg<FG_SETFONT, void*>(this, font); }

size_t FG_FASTCALL fgElement::SetLineHeight(float lineheight) { return _sendmsg<FG_SETLINEHEIGHT, float>(this, lineheight); }

size_t FG_FASTCALL fgElement::SetLetterSpacing(float letterspacing) { return _sendmsg<FG_SETLETTERSPACING, float>(this, letterspacing); }

size_t FG_FASTCALL fgElement::SetText(const char* text, FGSETTEXT mode) { return _sendsubmsg<FG_SETTEXT, const void*>(this, mode, text); }

void* fgElement::GetResource() { return reinterpret_cast<void*>(_sendmsg<FG_GETRESOURCE>(this)); }

const CRect* fgElement::GetUV() { return reinterpret_cast<const CRect*>(_sendmsg<FG_GETUV>(this)); }

unsigned int FG_FASTCALL fgElement::GetColor(FGSETCOLOR index) { return (unsigned int)_sendsubmsg<FG_GETCOLOR>(this, index); }

float fgElement::GetOutline() { return *reinterpret_cast<float*>(_sendmsg<FG_GETOUTLINE>(this)); }

void* fgElement::GetFont() { return reinterpret_cast<void*>(_sendmsg<FG_GETFONT>(this)); }

float fgElement::GetLineHeight() { size_t r = _sendmsg<FG_GETLINEHEIGHT>(this); return *reinterpret_cast<float*>(&r); }

float fgElement::GetLetterSpacing() { size_t r = _sendmsg<FG_GETLETTERSPACING>(this); return *reinterpret_cast<float*>(&r); }

const int* fgElement::GetText(FGSETTEXT mode) { return reinterpret_cast<const int*>(_sendmsg<FG_GETTEXT, ptrdiff_t>(this, mode)); }

void fgElement::AddListener(unsigned short type, fgListener listener) { fgElement_AddListener(this, type, listener); }