
/******************************************************************************
* MODULE     : virtual_font.cpp
* DESCRIPTION: fonts consisting of extra symbols which can be generated
*              automatically from a defining tree
* COPYRIGHT  : (C) 1999  Joris van der Hoeven
*******************************************************************************
* This software falls under the GNU general public license version 3 or later.
* It comes WITHOUT ANY WARRANTY WHATSOEVER. For details, see the file LICENSE
* in the root directory or <http://www.gnu.org/licenses/gpl-3.0.html>.
******************************************************************************/

#include "font.hpp"
#include "translator.hpp"
#include "analyze.hpp"
#include "frame.hpp"
#include "iterator.hpp"

/******************************************************************************
* The virtual font class
******************************************************************************/

struct virtual_font_rep: font_rep {
  font         base_fn;
  string       fn_name;
  translator   virt;
  int          size;
  int          hdpi;
  int          vdpi;
  bool         extend;
  int          last;
  font_metric  fnm;
  font_glyphs  fng;
  double       hunit;
  double       vunit;
  hashmap<scheme_tree,metric_struct> trm;
  hashmap<string,bool> sup;

  virtual_font_rep (string name, font base, string vname, int size,
                    int hdpi, int vdpi, bool extend);
  bool   supported (scheme_tree t);
  bool   supported (string c);
  glyph  compile_bis (scheme_tree t, metric& ex);
  glyph  compile (scheme_tree t, metric& ex);
  void   get_metric (scheme_tree t, metric& ex);
  tree   get_tree (string s);
  void   draw (renderer ren, scheme_tree t, SI x, SI y);
  void   draw_clipped (renderer ren, scheme_tree t, SI x, SI y,
                       SI x1, SI y1, SI x2, SI y2);
  void   draw_transformed (renderer ren, scheme_tree t, SI x, SI y, frame f);
  void   advance_glyph (string s, int& pos);
  int    get_char (string s, font_metric& fnm, font_glyphs& fng);
  glyph  get_glyph (string s);
  int    index_glyph (string s, font_metric& fnm, font_glyphs& fng);

  bool   supports (string c);
  void   get_extents (string s, metric& ex);
  void   get_xpositions (string s, SI* xpos);
  void   get_xpositions (string s, SI* xpos, bool lit);
  void   get_xpositions (string s, SI* xpos, SI xk);
  void   draw_fixed (renderer ren, string s, SI x, SI y);
  void   draw_fixed (renderer ren, string s, SI x, SI y, SI xk);
  font   magnify (double zoomx, double zoomy);

  double get_left_slope (string s);
  double get_right_slope (string s);
  SI     get_right_correction (string s);
};

virtual_font_rep::virtual_font_rep (
  string name, font base, string vname, int size2,
  int hdpi2, int vdpi2, bool extend2):
    font_rep (name, base), base_fn (base), fn_name (vname),
    virt (load_translator (vname)), size (size2),
    hdpi (hdpi2), vdpi (vdpi2), extend (extend2),
    last (N(virt->virt_def)),
    fnm (std_font_metric (name, tm_new_array<metric> (last), 0, last-1)),
    fng (std_font_glyphs (name, tm_new_array<glyph> (last), 0, last-1)),
    trm (metric_struct ()), sup (false)
{
  copy_math_pars (base_fn);
  hunit= ((size*hdpi)/72)*PIXEL;
  vunit= ((size*vdpi)/72)*PIXEL;
}

/******************************************************************************
* Check integrity of virtual character
******************************************************************************/

bool
virtual_font_rep::supported (scheme_tree t) {
  if (is_atomic (t)) {
    string r= t->label;
    if (r == "#28") r= "(";
    if (r == "#29") r= ")";
    if (N(r)>1) r= "<" * r * ">";
    if (!extend || base_fn->supports (r) || !virt->dict->contains (r))
      return base_fn->supports (r);
    if (!virt->dict->contains (r)) return false;
    return supported (virt->virt_def [virt->dict [r]]);
  }

  if (is_func (t, TUPLE, 3) && (is_double (t[0])) && (is_double (t[1])))
    return supported (t[2]);

  if (is_tuple (t, "or") && N(t) >= 2) {
    int i, n= N(t);
    for (i=1; i<n; i++)
      if (supported (t[i]))
        return true;
    return false;
  }

  if (is_tuple (t, "join") ||
      is_tuple (t, "glue", 2) ||
      is_tuple (t, "glue*", 2) ||
      is_tuple (t, "glue-above", 2) ||
      is_tuple (t, "glue-below", 2) ||
      is_tuple (t, "add", 2)) {
    int i, n= N(t);
    for (i=1; i<n; i++)
      if (!supported (t[i])) return false;
    return true;
  }

  if (is_tuple (t, "enlarge") ||
      is_tuple (t, "clip") ||
      is_tuple (t, "part") ||
      is_tuple (t, "hor-flip", 1) ||
      is_tuple (t, "ver-flip", 1) ||
      is_tuple (t, "rot-left", 1) ||
      is_tuple (t, "rot-right", 1) ||
      is_tuple (t, "hor-extend", 3) ||
      is_tuple (t, "hor-extend", 4) ||
      is_tuple (t, "ver-extend", 3) ||
      is_tuple (t, "ver-extend", 4) ||
      is_tuple (t, "ver-take", 3) ||
      is_tuple (t, "ver-take", 4) ||
      is_tuple (t, "italic", 3))
    return supported (t[1]);

  if (is_tuple (t, "align") && N(t) >= 3)
    return supported (t[1]) && supported (t[2]);

  return false;
}

bool
virtual_font_rep::supported (string c) {
  if (!sup->contains (c)) {
    tree t= get_tree (c);
    if (t == "") sup (c)= false;
    else sup (c)= supported (t);
  }
  return sup[c];
}

/******************************************************************************
* Compilation of virtual characters
******************************************************************************/

static void
outer_fit (metric& ex, metric& ey, SI x, SI y) {
  ex->x1= min (ex->x1, x+ ey->x1);
  ex->y1= min (ex->y1, y+ ey->y1);
  ex->x2= max (ex->x2, x+ ey->x2);
  ex->y2= max (ex->y2, y+ ey->y2);
  ex->x3= min (ex->x3, x+ ey->x3);
  ex->y3= min (ex->y3, y+ ey->y3);
  ex->x4= max (ex->x4, x+ ey->x4);
  ex->y4= max (ex->y4, y+ ey->y4);
}

static void
move (metric& ex, SI x, SI y) {
  if (x != 0) {
    ex->x1 += x; ex->x3 += x - PIXEL;
    ex->x2 += x; ex->x4 += x + PIXEL;
  }
  if (y != 0) {
    ex->y1 += y; ex->y3 += y - PIXEL;
    ex->y2 += y; ex->y4 += y + PIXEL;
  }
}

glyph
virtual_font_rep::compile_bis (scheme_tree t, metric& ex) {
  //cout << "Compile " << t << "\n";

  if (is_atomic (t)) {
    string r= t->label;
    if (r == "#28") r= "(";
    if (r == "#29") r= ")";
    if (N(r)>1) r= "<" * r * ">";
    glyph gl;
    if (!extend || base_fn->supports (r) || !virt->dict->contains (r)) {
      base_fn->get_extents (r, ex);
      gl= base_fn->get_glyph (r);
    }
    else {
      scheme_tree u= virt->virt_def [virt->dict [r]];
      gl= compile (u, ex);
    }
    if (gl->width == 0 && gl->height == 0)
      ex->x1= ex->y1= ex->x2= ex->y2= ex->x3= ex->y3= ex->x4= ex->y4= 0;
    return gl;
  }

  if (is_func (t, TUPLE, 3) &&
      (is_double (t[0])) && (is_double (t[1])))
    {
      SI x= (SI) (as_double (t[0]) * hunit);
      SI y= (SI) (as_double (t[1]) * vunit);
      glyph gl= compile (t[2], ex);
      move (ex, x, y);
      return move (gl, x, y);
    }

  if (is_tuple (t, "or") && N(t) >= 2) {
    int i, n= N(t);
    for (i=1; i<n-1; i++)
      if (supported (t[i]))
        return compile (t[i], ex);
    return compile (t[n-1], ex);
  }

  if (is_tuple (t, "join")) {
    int i, n= N(t);
    glyph gl1= compile (t[1], ex);
    for (i=2; i<n; i++) {
      metric ey;
      glyph gl2= compile (t[i], ey);
      outer_fit (ex, ey, 0, 0);
      gl1= join (gl1, gl2);
    }
    return gl1;
  }

  if (is_tuple (t, "glue", 2)) {
    metric ey;
    glyph gl1= compile (t[1], ex);
    glyph gl2= compile (t[2], ey);
    SI dx= ex->x2- ((base_fn->wpt*28)>>4);
    outer_fit (ex, ey, dx, 0);
    return join (gl1, move (gl2, dx, 0));
  }

  if (is_tuple (t, "glue*", 2)) {
    metric ey;
    glyph gl1= compile (t[1], ex);
    glyph gl2= compile (t[2], ey);
    SI dx= ex->x2;
    outer_fit (ex, ey, dx, 0);
    return join (gl1, move (gl2, dx, 0));
  }

  if (is_tuple (t, "glue-above", 2)) {
    metric ey;
    glyph gl1= compile (t[1], ex);
    glyph gl2= compile (t[2], ey);
    SI dy= ex->y2 - ey->y1;
    outer_fit (ex, ey, 0, dy);
    return join (gl1, move (gl2, 0, dy));
  }

  if (is_tuple (t, "glue-below", 2)) {
    metric ey;
    glyph gl1= compile (t[1], ex);
    glyph gl2= compile (t[2], ey);
    SI dy= ex->y1 - ey->y2;
    outer_fit (ex, ey, 0, dy);
    return join (gl1, move (gl2, 0, dy));
  }

  if (is_tuple (t, "add", 2)) {
    metric ey;
    glyph gl1= compile (t[1], ex);
    glyph gl2= compile (t[2], ey);
    SI dx= ((ex->x1+ ex->x2- ey->x1- ey->x2) >> 1);
    outer_fit (ex, ey, dx, 0);
    return join (gl1, move (gl2, dx, 0));
  }

  if (is_tuple (t, "enlarge")) {
    glyph gl= compile (t[1], ex);
    if (N(t)>2) ex->x1 -= (SI) (as_double (t[2]) * hunit);
    if (N(t)>3) ex->x2 += (SI) (as_double (t[3]) * hunit);
    if (N(t)>4) ex->y1 -= (SI) (as_double (t[4]) * vunit);
    if (N(t)>5) ex->y2 += (SI) (as_double (t[5]) * vunit);
    return gl;
  }

  if (is_tuple (t, "clip")) {
    glyph gl= compile (t[1], ex);
    SI x1, y1, x2, y2;
    get_bounding_box (gl, x1, y1, x2, y2);
    if (N(t)>2 && t[2]!="*")
      x1= ex->x1= ex->x3= (SI) (as_double (t[2]) * hunit);
    if (N(t)>3 && t[3]!="*")
      x2= ex->x2= ex->x4= (SI) (as_double (t[3]) * hunit);
    if (N(t)>4 && t[4]!="*")
      y1= ex->y1= ex->y3= (SI) (as_double (t[4]) * vunit);
    if (N(t)>5 && t[5]!="*")
      y2= ex->y2= ex->y4= (SI) (as_double (t[5]) * vunit);
    return clip (gl, x1, y1, x2, y2);
  }

  if (is_tuple (t, "part")) {
    glyph gl= compile (t[1], ex);
    SI ox= ex->x1, gw= ex->x2 - ex->x1;
    SI oy= ex->y1, gh= ex->y2 - ex->y1;
    SI x1, y1, x2, y2;
    get_bounding_box (gl, x1, y1, x2, y2);
    if (N(t)>2 && t[2]!="*")
      x1= ex->x1= ex->x3= ox + (SI) (as_double (t[2]) * gw);
    if (N(t)>3 && t[3]!="*")
      x2= ex->x2= ex->x4= ox + (SI) (as_double (t[3]) * gw);
    if (N(t)>4 && t[4]!="*")
      y1= ex->y1= ex->y3= oy + (SI) (as_double (t[4]) * gh);
    if (N(t)>5 && t[5]!="*")
      y2= ex->y2= ex->y4= oy + (SI) (as_double (t[5]) * gh);
    glyph cgl= clip (gl, x1, y1, x2, y2);
    SI dx= 0, dy= 0;
    if (N(t)>6) dx= (SI) (as_double (t[6]) * gw);
    if (N(t)>7) dy= (SI) (as_double (t[7]) * gh);
    if (dx == 0 && dy == 0) return cgl;
    if (dx != 0) {
      ex->x1 += dx; ex->x3 += dx - PIXEL;
      ex->x2 += dx; ex->x4 += dx + PIXEL;
    }
    if (dy != 0) {
      ex->y1 += dy; ex->y3 += dy - PIXEL;
      ex->y2 += dy; ex->y4 += dy + PIXEL;
    }
    return move (cgl, dx, dy);
  }

  if (is_tuple (t, "hor-flip", 1))
    return hor_flip (compile (t[1], ex));

  if (is_tuple (t, "ver-flip", 1))
    return ver_flip (compile (t[1], ex));

  if (is_tuple (t, "rot-left", 1)) {
    metric ey;
    glyph gl= pos_rotate (compile (t[1], ey));
    ex->x1= 0;
    ex->y1= 0;
    ex->x2= ey->y2- ey->y1;
    ex->y2= ey->x2- ey->x1;
    ex->x3= ey->y2- ey->y4;
    ex->y3= ey->x3- ey->x1;
    ex->x4= ey->y2- ey->y3;
    ex->y4= ey->x4- ey->x1;
    return move (gl, ey->y2, -ey->x1);
  }

  if (is_tuple (t, "rot-right", 1)) {
    metric ey;
    glyph gl= pos_rotate (pos_rotate (pos_rotate (compile (t[1], ey))));
    ex->x1= 0;
    ex->y1= 0;
    ex->x2= ey->y2- ey->y1;
    ex->y2= ey->x2- ex->x1;
    ex->x3= ey->y3- ey->y1;
    ex->y3= ey->x2- ey->x4;
    ex->x4= ey->y4- ey->y1;
    ex->y4= ey->x2- ey->x3;
    return move (gl, -ey->y1, ey->x2);
  }

  if (is_tuple (t, "hor-extend", 3) || is_tuple (t, "hor-extend", 4)) {
    glyph gl= compile (t[1], ex);
    int pos= (int) (as_double (t[2]) * gl->width);
    SI  add= (SI)  (as_double (t[3]) * hunit);
    if (is_tuple (t, "hor-extend", 4))
      add= (SI)  (as_double (t[3]) * as_double (t[4]) * hunit);
    int by = add / PIXEL;
    if (pos < 0) pos= 0;
    if (pos >= gl->width) pos= gl->width-1;
    ex->x2 += add;
    ex->x4 += by * PIXEL;
    return hor_extend (gl, pos, by);
  }

  if (is_tuple (t, "ver-extend", 3) || is_tuple (t, "ver-extend", 4)) {
    glyph gl= compile (t[1], ex);
    int pos= (int) ((1.0 - as_double (t[2])) * gl->height);
    SI  add= (SI)  (as_double (t[3]) * vunit);
    if (is_tuple (t, "ver-extend", 4))
      add= (SI)  (as_double (t[3]) * as_double (t[4]) * vunit);
    int by = add / PIXEL;
    if (pos < 0) pos= 0;
    if (pos >= gl->height) pos= gl->height-1;
    ex->y1 -= add;
    ex->y3 -= by * PIXEL;
    return ver_extend (gl, pos, by);
  }

  if (is_tuple (t, "ver-take", 3) || is_tuple (t, "ver-take", 4)) {
    glyph gl= compile (t[1], ex);
    int pos= (int) ((1.0 - as_double (t[2])) * gl->height);
    SI  add= (SI)  (as_double (t[3]) * (ex->y2 - ex->y1));
    if (is_tuple (t, "ver-take", 4))
      add= (SI) (as_double (t[3]) * as_double (t[4]) * (ex->y2 - ex->y1));
    int nr = add / PIXEL;
    if (pos < 0) pos= 0;
    if (pos >= gl->height) pos= gl->height-1;
    ex->y1= -add;
    ex->y2= 0;
    ex->y3= -nr * PIXEL;
    ex->y4= 0;
    return ver_take (gl, pos, nr);
  }

  if (is_tuple (t, "align") && N(t) >= 3) {
    metric ex2;
    glyph gl= compile (t[1], ex);
    glyph gl2= compile (t[2], ex2);
    double xa= 0.0, xa2= 0.0, ya= 0.0, ya2= 0.0;
    if (N(t) >= 4 && is_double (t[3])) xa= xa2= as_double (t[3]);
    if (N(t) >= 5 && is_double (t[4])) ya= ya2= as_double (t[4]);
    if (N(t) >= 6 && is_double (t[5])) xa2= as_double (t[5]);
    if (N(t) >= 7 && is_double (t[6])) ya2= as_double (t[6]);
    SI ax = (SI) (ex ->x1 + xa  * (ex ->x2 - ex ->x1));
    SI ax2= (SI) (ex2->x1 + xa2 * (ex2->x2 - ex2->x1));
    SI ay = (SI) (ex ->y1 + ya  * (ex ->y2 - ex ->y1));
    SI ay2= (SI) (ex2->y1 + ya2 * (ex2->y2 - ex2->y1));
    SI dx = ax2 - ax;
    SI dy = ay2 - ay;
    if (N(t) >= 4 && t[3] == "*") dx= 0;
    if (N(t) >= 5 && t[4] == "*") dy= 0;
    move (ex, dx, dy);
    return move (gl, dx, dy);
  }

  if (is_tuple (t, "italic", 3))
    return compile (t[1], ex);

  failed_error << "TeXmacs] The defining tree is " << t << "\n";
  FAILED ("invalid virtual character");
  return glyph ();
}

glyph
virtual_font_rep::compile (scheme_tree t, metric& ex) {
  glyph r= compile_bis (t, ex);
  trm(t)= ex[0];
  return r;
}

void
virtual_font_rep::get_metric (scheme_tree t, metric& ex) {
  if (trm->contains (t)) ex[0]= trm[t];
  else (void) compile (t, ex);
}

/******************************************************************************
* Direct drawing of virtual fonts using vector graphics
******************************************************************************/

void
virtual_font_rep::draw (renderer ren, scheme_tree t, SI x, SI y) {
  if (is_atomic (t)) {
    string r= t->label;
    if (r == "#28") r= "(";
    if (r == "#29") r= ")";
    if (N(r)>1) r= "<" * r * ">";
    if (!extend || base_fn->supports (r) || !virt->dict->contains (r))
      base_fn->draw (ren, r, x, y);
    else {
      scheme_tree u= virt->virt_def [virt->dict [r]];
      draw (ren, u, x, y);
    }
    return;
  }
  
  if (is_func (t, TUPLE, 3) &&
      (is_double (t[0])) && (is_double (t[1])))
    {
      SI dx= (SI) (as_double (t[0]) * hunit);
      SI dy= (SI) (as_double (t[1]) * vunit);
      draw (ren, t[2], x+dx, y+dy);
      return;
    }

  if (is_tuple (t, "or") && N(t) >= 2) {
    int i, n= N(t);
    for (i=1; i<n-1; i++)
      if (supported (t[i])) {
        draw (ren, t[i], x, y);
        return;
      }
    draw (ren, t[n-1], x, y);
    return;
  }

  if (is_tuple (t, "join")) {
    int i, n= N(t);
    for (i=1; i<n; i++)
      draw (ren, t[i], x, y);
    return;
  }

  if (is_tuple (t, "glue", 2)) {
    metric ex;
    get_metric (t[1], ex);
    SI dx= ex->x2- ((base_fn->wpt*28)>>4);
    draw (ren, t[1], x, y);
    draw (ren, t[2], x + dx, y);
    return;
  }

  if (is_tuple (t, "glue*", 2)) {
    metric ex;
    get_metric (t[1], ex);
    SI dx= ex->x2;
    draw (ren, t[1], x, y);
    draw (ren, t[2], x + dx, y);
    return;
  }

  if (is_tuple (t, "glue-above", 2)) {
    metric ex, ey;
    get_metric (t[1], ex);
    get_metric (t[2], ey);
    SI dy= ex->y2 - ey->y1;
    draw (ren, t[1], x, y);
    draw (ren, t[2], x, y + dy);
    return;
  }

  if (is_tuple (t, "glue-below", 2)) {
    metric ex, ey;
    get_metric (t[1], ex);
    get_metric (t[2], ey);
    SI dy= ex->y1 - ey->y2;
    draw (ren, t[1], x, y);
    draw (ren, t[2], x, y + dy);
    return;
  }

  if (is_tuple (t, "add", 2)) {
    metric ex, ey;
    get_metric (t[1], ex);
    get_metric (t[2], ey);
    SI dx= ((ex->x1+ ex->x2- ey->x1- ey->x2) >> 1);
    draw (ren, t[1], x, y);
    draw (ren, t[2], x + dx, y);
    return;
  }

  if (is_tuple (t, "enlarge")) {
    draw (ren, t[1], x, y);
    return;
  }

  if (is_tuple (t, "clip")) {
    metric ex;
    get_metric (t[1], ex);
    if (N(t)>2 && t[2]!="*") ex->x3= (SI) (as_double (t[2]) * hunit);
    if (N(t)>3 && t[3]!="*") ex->x4= (SI) (as_double (t[3]) * hunit);
    if (N(t)>4 && t[4]!="*") ex->y3= (SI) (as_double (t[4]) * vunit);
    if (N(t)>5 && t[5]!="*") ex->y4= (SI) (as_double (t[5]) * vunit);
    draw_clipped (ren, t[1], x, y, ex->x3, ex->y3, ex->x4, ex->y4);
    return;
  }

  if (is_tuple (t, "part")) {
    metric ex;
    get_metric (t[1], ex);
    SI ox= ex->x1, gw= ex->x2 - ex->x1;
    SI oy= ex->y1, gh= ex->y2 - ex->y1;
    if (N(t)>2 && t[2]!="*") ex->x1= ex->x3= ox + (SI) (as_double (t[2]) * gw);
    if (N(t)>3 && t[3]!="*") ex->x2= ex->x4= ox + (SI) (as_double (t[3]) * gw);
    if (N(t)>4 && t[4]!="*") ex->y1= ex->y3= oy + (SI) (as_double (t[4]) * gh);
    if (N(t)>5 && t[5]!="*") ex->y2= ex->y4= oy + (SI) (as_double (t[5]) * gh);
    SI dx= 0, dy= 0;
    if (N(t)>6) dx= (SI) (as_double (t[6]) * gw);
    if (N(t)>7) dy= (SI) (as_double (t[7]) * gh);
    draw_clipped (ren, t[1], x + dx, y + dy, ex->x3, ex->y3, ex->x4, ex->y4);
    return;
  }

  if (is_tuple (t, "hor-flip", 1)) {
    metric ex;
    get_metric (t[1], ex);
    SI ox= x + ex->x3 + ex->x4;
    frame f= scaling (point (-1.0, 1.0), point ((double) ox, 0.0));
    draw_transformed (ren, t[1], 0, y, f);
    return;
  }

  if (is_tuple (t, "ver-flip", 1)) {
    metric ex;
    get_metric (t[1], ex);
    SI oy= y + ex->y3 + ex->y4;
    frame f= scaling (point (1.0, -1.0), point (0.0, (double) oy));
    draw_transformed (ren, t[1], x, 0, f);
    return;
  }

  if (is_tuple (t, "rot-left", 1)) {
    // FIXME: check that we should not use physical metrics
    // as in the case of hor-flip and ver-flip
    metric ex;
    get_metric (t[1], ex);
    //cout << "left " << (x/PIXEL) << ", " << (y/PIXEL) << "; "
    //     << (ex->x1/PIXEL) << ", " << (ex->y1/PIXEL) << "; "
    //     << (ex->x2/PIXEL) << ", " << (ex->y2/PIXEL) << "\n";
    SI ox= x + ex->x1;
    SI oy= y + ex->y2;
    frame f= rotation_2D (point (ox, oy), 1.57079632679);
    draw_transformed (ren, t[1], x - ex->y2, y + ex->x1, f);
    return;
  }

  if (is_tuple (t, "rot-right", 1)) {
    // FIXME: check that we should not use physical metrics
    // as in the case of hor-flip and ver-flip
    metric ex;
    get_metric (t[1], ex);
    //cout << "right " << (x/PIXEL) << ", " << (y/PIXEL) << "; "
    //     << (ex->x1/PIXEL) << ", " << (ex->y1/PIXEL) << "; "
    //     << (ex->x2/PIXEL) << ", " << (ex->y2/PIXEL) << "\n";
    SI ox= x + ex->x2;
    SI oy= y + ex->y1;
    frame f= rotation_2D (point (ox, oy), -1.57079632679);
    draw_transformed (ren, t[1], x + ex->y1, y - ex->x2, f);
    return;
  }

  if (is_tuple (t, "hor-extend", 3) || is_tuple (t, "hor-extend", 4)) {
    metric ex;
    get_metric (t[1], ex);
    SI pos= (SI) (as_double (t[2]) * (ex->x2 - ex->x1));
    SI add= (SI) (as_double (t[3]) * hunit);
    if (is_tuple (t, "hor-extend", 4))
      add= (SI) (as_double (t[3]) * as_double (t[4]) * hunit);
    if (add > 0 && ex->x2 > ex->x1) {
      SI  w = ex->x2 - ex->x1;
      int n = (int) ((20 * add + w - 1) / w);
      SI  dx= (add + n - 1) / n;
      SI  hx= (add + 2*n - 1) / (2*n);
      for (int i=0; i<n; i++)
        draw_clipped (ren, t[1], x + hx + i*dx, y,
                      ex->x3 + pos - hx, ex->y3, ex->x3 + pos + hx, ex->y4);
    }
    draw_clipped (ren, t[1], x, y, ex->x3, ex->y3, ex->x3 + pos, ex->y4);
    draw_clipped (ren, t[1], x + add, y, ex->x3 + pos, ex->y3, ex->x4, ex->y4);
    return;
  }

  if (is_tuple (t, "ver-extend", 3) || is_tuple (t, "ver-extend", 4)) {
    metric ex;
    get_metric (t[1], ex);
    SI pos= (SI) ((1.0 - as_double (t[2])) * (ex->y2 - ex->y1));
    SI add= (SI) (as_double (t[3]) * vunit);
    if (is_tuple (t, "ver-extend", 4))
      add= (SI) (as_double (t[3]) * as_double (t[4]) * vunit);
    if (add > 0 && ex->y2 > ex->y1) {
      SI  h = ex->y2 - ex->y1;
      int n = (int) ((20 * add + h - 1) / h);
      SI  dy= (add + n - 1) / n;
      SI  hy= (add + 2*n - 1) / (2*n);
      for (int i=0; i<n; i++)
        draw_clipped (ren, t[1], x, y + hy + i*dy - add,
                      ex->x3, ex->y3 + pos - hy, ex->x4, ex->y3 + pos + hy);
    }
    draw_clipped (ren, t[1], x, y - add, ex->x3, ex->y3, ex->x4, ex->y3 + pos);
    draw_clipped (ren, t[1], x, y, ex->x3, ex->y3 + pos, ex->x4, ex->y4);
    return;
  }

  if (is_tuple (t, "ver-take", 3) || is_tuple (t, "ver-take", 4)) {
    metric ex;
    get_metric (t[1], ex);
    SI pos= (SI) ((1.0 - as_double (t[2])) * (ex->y2 - ex->y1));
    SI add= (SI) (as_double (t[3]) * (ex->y2 - ex->y1));
    if (is_tuple (t, "ver-take", 4))
      add= (SI) (as_double (t[3]) * as_double (t[4]) * (ex->y2 - ex->y1));
    if (add > 0 && ex->y2 > ex->y1) {
      SI  h = ex->y2 - ex->y1;
      int n = (int) ((20 * add + h - 1) / h);
      SI  dy= (add + n - 1) / n;
      SI  hy= (add + 2*n - 1) / (2*n);
      for (int i=0; i<n; i++)
        draw_clipped (ren, t[1], x, y + i*dy - add - (ex->y3 + pos),
                      ex->x3, ex->y3 + pos - hy, ex->x4, ex->y3 + pos + hy);
    }
    return;
  }

  if (is_tuple (t, "align") && N(t) >= 3) {
    metric ex, ex2;
    get_metric (t[1], ex);
    get_metric (t[2], ex2);
    double xa= 0.0, xa2= 0.0, ya= 0.0, ya2= 0.0;
    if (N(t) >= 4 && is_double (t[3])) xa= xa2= as_double (t[3]);
    if (N(t) >= 5 && is_double (t[4])) ya= ya2= as_double (t[4]);
    if (N(t) >= 6 && is_double (t[5])) xa2= as_double (t[5]);
    if (N(t) >= 7 && is_double (t[6])) ya2= as_double (t[6]);
    SI ax = (SI) (ex ->x1 + xa  * (ex ->x2 - ex ->x1));
    SI ax2= (SI) (ex2->x1 + xa2 * (ex2->x2 - ex2->x1));
    SI ay = (SI) (ex ->y1 + ya  * (ex ->y2 - ex ->y1));
    SI ay2= (SI) (ex2->y1 + ya2 * (ex2->y2 - ex2->y1));
    SI dx = ax2 - ax;
    SI dy = ay2 - ay;
    if (N(t) >= 4 && t[3] == "*") dx= 0;
    if (N(t) >= 5 && t[4] == "*") dy= 0;
    draw (ren, t[1], x+dx, y+dy);
    return;
  }

  if (is_tuple (t, "italic", 3)) {
    draw (ren, t[1], x, y);
    return;
  }
}

void
virtual_font_rep::draw_clipped (renderer ren, scheme_tree t, SI x, SI y,
                                SI x1, SI y1, SI x2, SI y2) {
  ren->clip (x + x1, y + y1, x + x2, y + y2);
  draw (ren, t, x, y);
  ren->unclip ();  
}

void
virtual_font_rep::draw_transformed (renderer ren, scheme_tree t, SI x, SI y,
                                    frame f) {
  ren->set_transformation (f);
  draw (ren, t, x, y);
  ren->reset_transformation ();  
}

/******************************************************************************
* Getting extents and drawing strings
******************************************************************************/

bool is_hex_digit (char c);

static tree
subst_sharp (tree t, string by) {
  if (is_atomic (t)) {
    int i;
    string s= t->label;
    i= search_forwards ("#", s);
    if (i == -1) return s;
    else if (i == 0 && N(s) >= 2 && is_hex_digit (s[1])) return s;
    else return s(0,i) * by * s(i+1,N(s));
  }
  else {
    int i, n= N(t);
    tree r (t, n);
    for (i=0; i<n; i++)
      r[i]= subst_sharp (t[i], by);
    return r;
  }
}

static void
make_char_font (string name, font_metric& cfnm, font_glyphs& cfng) {
  cfnm= std_font_metric (name, tm_new_array<metric> (1), 0, 0);
  cfng= std_font_glyphs (name, tm_new_array<glyph> (1), 0, 0);
}

/******************************************************************************
* Getting extents and drawing strings
******************************************************************************/

int
virtual_font_rep::get_char (string s, font_metric& cfnm, font_glyphs& cfng) {
  if (N(s) == 0) return -1;
  if (N(s) == 1) {
    int c= ((QN) s[0]);
    if ((c<0) || (c>=last)) return -1;
    cfnm= fnm;
    cfng= fng;
    if (is_nil (fng->get(c)))
      fng->get(c)= compile (virt->virt_def[c], fnm->get(c));
    return c;
  }
  else if (s[0] == '<' && s[N(s)-1] == '>') {
    if (!virt->dict->contains (s)) return -1;
    int c2= virt->dict [s];
    cfnm= fnm;
    cfng= fng;
    if (is_nil (fng->get(c2)))
      fng->get(c2)= compile (virt->virt_def[c2], fnm->get(c2));
    return c2;
  }
  else {
    int c= ((QN) s[0]);
    if ((c<0) || (c>=last)) return -1;
    string sub= "[" * as_string (c) * "," * s(1,N(s)-1) * "]";
    make_char_font (res_name * sub, cfnm, cfng);
    tree t= subst_sharp (virt->virt_def[c], s(1,N(s)));
    if (is_nil (cfng->get(0)))
      cfng->get(0)= compile (t, cfnm->get(0));
    return 0;
  }
}

tree
virtual_font_rep::get_tree (string s) {
  if (s == "") return "";
  int c= ((QN) s[0]);
  if (s[0] == '<' && s[N(s)-1] == '>') {
    if (!virt->dict->contains (s)) return "";
    int c2= virt->dict [s];
    return virt->virt_def[c2];
  }
  else if ((c<0) || (c>=last)) return "";
  else if (N(s)==1) return virt->virt_def[c];
  else return subst_sharp (virt->virt_def[c], s(1,N(s)));
}

bool
virtual_font_rep::supports (string s) {
  if (extend && base_fn->supports (s)) return true;
  return supported (s);
}

void
virtual_font_rep::get_extents (string s, metric& ex) {
  if (extend && base_fn->supports (s)) {
    base_fn->get_extents (s, ex); return; }
  font_metric cfnm;
  font_glyphs cfng;
  int c= get_char (s, cfnm, cfng);
  if (c == -1) {
    ex->y1= y1; ex->y2= y2;
    ex->x1= ex->x2= ex->x3= ex->x4= ex->y3= ex->y4= 0;
  }
  else {
    metric_struct* ey= cfnm->get(c);
    ex->x1= ey->x1; ex->y1= ey->y1;
    ex->x2= ey->x2; ex->y2= ey->y2;
    ex->x3= ey->x3; ex->y3= ey->y3;
    ex->x4= ey->x4; ex->y4= ey->y4;
  }
}

void
virtual_font_rep::get_xpositions (string s, SI* xpos) {
  get_xpositions (s, xpos, 0);
}

void
virtual_font_rep::get_xpositions (string s, SI* xpos, bool lit) {
  (void) lit;
  get_xpositions (s, xpos, 0);
}

void
virtual_font_rep::get_xpositions (string s, SI* xpos, SI xk) {
  if (extend && base_fn->supports (s)) {
    base_fn->get_xpositions (s, xpos, xk); return; }
  metric ex;
  get_extents (s, ex);
  xpos[0]= xk;
  xpos[N(s)]= ex->x2 + xk;
  for (int i=1; i<N(s); i++)
    xpos[i]= (xpos[0] + xpos[N(s)]) >> 1;
}

void
virtual_font_rep::draw_fixed (renderer ren, string s, SI x, SI y) {
  if (extend && base_fn->supports (s))
    base_fn->draw_fixed (ren, s, x, y);
  else if (ren->is_screen) {
    font_metric cfnm;
    font_glyphs cfng;
    int c= get_char (s, cfnm, cfng);
    if (c != -1) ren->draw (c, cfng, x, y);
  }
  else {
    tree t= get_tree (s);
    if (t != "") draw (ren, t, x, y);
  }
}

void
virtual_font_rep::draw_fixed (renderer ren, string s, SI x, SI y, SI xk) {
  draw_fixed (ren, s, x+xk, y);
}

font
virtual_font_rep::magnify (double zoomx, double zoomy) {
  return virtual_font (base_fn->magnify (zoomx, zoomy), fn_name, size,
                       (int) tm_round (hdpi * zoomx),
                       (int) tm_round (vdpi * zoomy), extend);
}

void
virtual_font_rep::advance_glyph (string s, int& pos) {
  pos= N(s);
}

glyph
virtual_font_rep::get_glyph (string s) {
  if (extend && base_fn->supports (s))
    return base_fn->get_glyph (s);
  font_metric cfnm;
  font_glyphs cfng;
  int c= get_char (s, cfnm, cfng);
  if (c == -1) return font_rep::get_glyph (s);
  else return cfng->get(c);
}

int
virtual_font_rep::index_glyph (string s, font_metric& cfnm,
                                         font_glyphs& cfng) {
  if (extend && base_fn->supports (s))
    return base_fn->index_glyph (s, cfnm, cfng);
  return get_char (s, cfnm, cfng);
}

/******************************************************************************
* Slope and italic correction
******************************************************************************/

double
virtual_font_rep::get_left_slope (string s) {
  if (extend && base_fn->supports (s))
    return base_fn->get_left_slope (s);
  tree t= get_tree (s);
  if (is_tuple (t, "italic", 3))
    return as_double (t[2]);
  return font_rep::get_left_slope (s);
}

double
virtual_font_rep::get_right_slope (string s) {
  if (extend && base_fn->supports (s))
    return base_fn->get_right_slope (s);
  tree t= get_tree (s);
  if (is_tuple (t, "italic", 3))
    return as_double (t[2]);
  return font_rep::get_right_slope (s);
}

SI
virtual_font_rep::get_right_correction (string s) {
  if (extend && base_fn->supports (s))
    return base_fn->get_right_correction (s);
  tree t= get_tree (s);
  if (is_tuple (t, "italic", 3))
    return (SI) (as_double (t[3]) * hunit);
  return font_rep::get_right_correction (s);
}

/******************************************************************************
* User interface
******************************************************************************/

static hashmap<string,bool> vdefined (false);

bool
virtually_defined (string c, string name) {
  if (!vdefined->contains (name)) {
    vdefined (name)= true;
    translator virt= load_translator (name);
    iterator<string> it= iterate (virt->dict);
    while (it->busy ())
      vdefined (name * "-" * it->next ())= true;
  }
  return vdefined [name * "-" * c];
}

font
virtual_font (font base, string name, int size,
              int hdpi, int vdpi, bool extend) {
  string full_name=
    base->res_name * (extend? string ("#enhance-"): string ("#virtual-")) *
    name * as_string (size) * "@" * as_string (hdpi);
  if (vdpi != hdpi) full_name << "x" << vdpi;
  return make (font, full_name,
               tm_new<virtual_font_rep> (full_name, base, name, size,
                                         hdpi, vdpi, extend));
}
