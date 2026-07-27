// preview.c — host-side mock-up of the Zwift Remote screen.
//
// WHAT THIS IS AND IS NOT
//   It reuses the REAL font headers and the REAL palette/geometry constants, and
//   its drawChar is a faithful port of Arduino_GFX::drawChar's custom-font path.
//   So it does verify the things that are actually new and risky: that the font
//   converter's bit packing is right, that labels fit their buttons, and that
//   nothing collides.
//   It re-implements the drawing PRIMITIVES rather than calling Arduino_GFX, so
//   it does not prove those. They were already working.
//
//   cc preview.c -o preview -lm && ./preview > screen.ppm

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "ZwiftBold26.h"
#include "ZwiftBold18.h"
#include "ZwiftBold14.h"
#include "ZwiftSmall11.h"

#define W 222
#define H 480
static uint16_t fb[H][W];

static uint16_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

static const uint16_t C_BG      = 0, C_SURFACE = 0, C_LINE = 0; // filled in main
static uint16_t P_BG, P_SURFACE, P_LINE, P_MUTED, P_WHITE;
static uint16_t P_ORANGE, P_BLUE, P_GREEN, P_AMBER, P_RED, P_STEEL;

static uint16_t mix565(uint16_t a, uint16_t b, uint8_t t) {
  int ar=(a>>11)&0x1F, ag=(a>>5)&0x3F, ab=a&0x1F;
  int br=(b>>11)&0x1F, bg=(b>>5)&0x3F, bb=b&0x1F;
  int r=ar+((br-ar)*t)/255, g=ag+((bg-ag)*t)/255, l=ab+((bb-ab)*t)/255;
  return (r<<11)|(g<<5)|l;
}

static uint16_t inkOn(uint16_t bg){
  int r=((bg>>11)&0x1F)*255/31,g=((bg>>5)&0x3F)*255/63,b=(bg&0x1F)*255/31;
  return ((77*r+151*g+28*b)>>8) > 150 ? P_BG : P_WHITE;
}
static void px(int x, int y, uint16_t c) { if (x>=0&&x<W&&y>=0&&y<H) fb[y][x]=c; }
static void fillRect(int x,int y,int w,int h,uint16_t c){ for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) px(i,j,c); }
static void fillScreen(uint16_t c){ fillRect(0,0,W,H,c); }
static void drawFastHLine(int x,int y,int w,uint16_t c){ fillRect(x,y,w,1,c); }

static int inRound(int px_,int py_,int x,int y,int w,int h,int r){
  if(px_<x||py_<y||px_>=x+w||py_>=y+h) return 0;
  int cx,cy;
  if(px_<x+r) cx=x+r; else if(px_>=x+w-r) cx=x+w-1-r; else return 1;
  if(py_<y+r) cy=y+r; else if(py_>=y+h-r) cy=y+h-1-r; else return 1;
  int dx=px_-cx, dy=py_-cy;
  return dx*dx+dy*dy <= r*r;
}
static void fillRoundRect(int x,int y,int w,int h,int r,uint16_t c){
  for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) if(inRound(i,j,x,y,w,h,r)) px(i,j,c);
}
static void drawRoundRect(int x,int y,int w,int h,int r,uint16_t c){
  for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++)
    if(inRound(i,j,x,y,w,h,r) && !inRound(i,j,x+1,y+1,w-2,h-2,r>0?r-1:0)) px(i,j,c);
}
static void drawLine(int x0,int y0,int x1,int y1,uint16_t c){
  int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy;
  for(;;){ px(x0,y0,c); if(x0==x1&&y0==y1) break; int e2=2*err;
    if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} }
}
static void drawCircle(int cx,int cy,int r,uint16_t c){
  for(int j=cy-r-1;j<=cy+r+1;j++) for(int i=cx-r-1;i<=cx+r+1;i++){
    double d=sqrt((i-cx)*(i-cx)+(j-cy)*(j-cy)); if(fabs(d-r)<0.6) px(i,j,c); }
}
static void fillCircle(int cx,int cy,int r,uint16_t c){
  for(int j=cy-r;j<=cy+r;j++) for(int i=cx-r;i<=cx+r;i++)
    if((i-cx)*(i-cx)+(j-cy)*(j-cy)<=r*r) px(i,j,c);
}
static double edge(double ax,double ay,double bx,double by,double cx,double cy){
  return (bx-ax)*(cy-ay)-(by-ay)*(cx-ax);
}
static void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){
  int minx=x0<x1?(x0<x2?x0:x2):(x1<x2?x1:x2), maxx=x0>x1?(x0>x2?x0:x2):(x1>x2?x1:x2);
  int miny=y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2), maxy=y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2);
  for(int j=miny;j<=maxy;j++) for(int i=minx;i<=maxx;i++){
    double a=edge(x0,y0,x1,y1,i,j), b=edge(x1,y1,x2,y2,i,j), d=edge(x2,y2,x0,y0,i,j);
    if((a>=0&&b>=0&&d>=0)||(a<=0&&b<=0&&d<=0)) px(i,j,c);
  }
}

// --- text: faithful port of Arduino_GFX::drawChar / charBounds ---------------
static const GFXfont *curFont;
static void setFont(const GFXfont *f){ curFont=f; }

static void drawChar(int x,int y,unsigned char ch,uint16_t color){
  if(ch<curFont->first||ch>curFont->last) return;
  const GFXglyph *g=&curFont->glyph[ch-curFont->first];
  const uint8_t *bm=curFont->bitmap;
  uint16_t bo=g->bitmapOffset;
  uint8_t bits=0,bit=0;
  for(int yy=0;yy<g->height;yy++)
    for(int xx=0;xx<g->width;xx++,bits<<=1){
      if(!(bit++&7)) bits=bm[bo++];
      if(bits&0x80) px(x+g->xOffset+xx, y+g->yOffset+yy, color);
    }
}
static void textAt(const char *s,const GFXfont *f,int x,int baseline,uint16_t c){
  setFont(f);
  for(const char *p=s;*p;p++){ drawChar(x,baseline,(unsigned char)*p,c); x+=f->glyph[(unsigned char)*p-f->first].xAdvance; }
}
static void bounds(const char *s,const GFXfont *f,int *x1,int *y1,int *w,int *h){
  int x=0,minx=1e5,miny=1e5,maxx=-1e5,maxy=-1e5;
  for(const char *p=s;*p;p++){
    const GFXglyph *g=&f->glyph[(unsigned char)*p-f->first];
    int a=x+g->xOffset,b=0+g->yOffset,cc=a+g->width-1,d=b+g->height-1;
    if(a<minx)minx=a; if(b<miny)miny=b; if(cc>maxx)maxx=cc; if(d>maxy)maxy=d;
    x+=g->xAdvance;
  }
  *x1=minx; *y1=miny; *w=maxx-minx+1; *h=maxy-miny+1;
}
static int textWidth(const char *s,const GFXfont *f){ int a,b,w,h; bounds(s,f,&a,&b,&w,&h); return w; }
static void textRight(const char *s,const GFXfont *f,int rx,int bl,uint16_t c){ textAt(s,f,rx-textWidth(s,f),bl,c); }
static void textCentred(const char *s,const GFXfont *f,int cx,int cy,uint16_t c){
  int x1,y1,w,h; bounds(s,f,&x1,&y1,&w,&h);
  textAt(s,f,cx-w/2-x1,cy-h/2-y1,c);
}
static const GFXfont *fitLabel(const char *s,int maxW){
  return textWidth(s,&ZwiftBold26)<=maxW ? &ZwiftBold26 : &ZwiftBold18;
}

// --- layout (mirrors src/main.cpp) ------------------------------------------
enum Glyph { G_None,G_Up,G_Down,G_Left,G_Right,G_Check,G_Thumb,G_Bike };
enum Style { S_Solid, S_Outline };
enum Kind  { K_Key, K_Auto };

#define CW 68
#define CH 68
#define COL0 4
#define COL1 77
#define COL2 150
#define ROW0 94
#define ROW1 167
#define ROW2 240
#define BOT_Y 316
#define BOT_H 78
#define BOT_W 104
#define HEADER_H 88
#define RADIUS 10
#define RIDE_ON_INTERVAL_MS 10000

typedef struct { int x,y,w,h; const char*label; const char*caption; int glyph; uint16_t color; int kind, style; } Pad;
static Pad pad[9];
static int autoRideOn=0, rideOnCount=0, lastWakeMs=224, bondsCleared=0;
static int connectedFlag=1;

static void drawArrow(int g,int cx,int cy,int s,uint16_t c){
  const int T=6;
  switch(g){
    case G_Up:    fillTriangle(cx,cy-s,cx-s,cy,cx+s,cy,c); fillRect(cx-T,cy,2*T,s,c); break;
    case G_Down:  fillTriangle(cx,cy+s,cx-s,cy,cx+s,cy,c); fillRect(cx-T,cy-s,2*T,s,c); break;
    case G_Left:  fillTriangle(cx-s,cy,cx,cy-s,cx,cy+s,c); fillRect(cx,cy-T,s,2*T,c); break;
    case G_Right: fillTriangle(cx+s,cy,cx,cy-s,cx,cy+s,c); fillRect(cx-s,cy-T,s,2*T,c); break;
  }
}
static void thickSeg(int x0,int y0,int x1,int y1,int t,uint16_t c){
  fillTriangle(x0,y0,x1,y1,x1,y1+t,c);
  fillTriangle(x0,y0,x1,y1+t,x0,y0+t,c);
}
static void drawCheck(int cx,int cy,uint16_t c){
  thickSeg(cx-16,cy-2,cx-5,cy+9,8,c);
  thickSeg(cx-5,cy+9,cx+16,cy-13,8,c);
}
static void drawThumbUp(int cx,int cy,uint16_t c,uint16_t fill){
  fillRoundRect(cx-17,cy-2,34,20,6,c);
  fillRoundRect(cx-14,cy-19,12,19,5,c);
  fillRect(cx-1,cy+4,14,1,fill);
  fillRect(cx-1,cy+11,14,1,fill);
}
static void stroke2(int x0,int y0,int x1,int y1,uint16_t c){
  drawLine(x0,y0,x1,y1,c); drawLine(x0+1,y0,x1+1,y1,c); drawLine(x0,y0+1,x1,y1+1,c);
}
static void drawBike(int cx,int cy,uint16_t c){
  int rh=cx-22, fh=cx+22, hy=cy+7, bb=cx-1, st=cx-9, sty=cy-9, ht=cx+13, hty=cy-11;
  for(int r=12;r>=11;r--){ drawCircle(rh,hy,r,c); drawCircle(fh,hy,r,c); }
  stroke2(rh,hy,st,sty,c); stroke2(st,sty,bb,hy,c); stroke2(bb,hy,rh,hy,c);
  stroke2(st,sty,ht,hty,c); stroke2(ht,hty,bb,hy,c); stroke2(ht,hty,fh,hy,c);
  stroke2(cx-16,sty-3,cx-4,sty-3,c);
  stroke2(ht-2,hty-3,ht+8,hty-3,c);
}

static void drawPadButton(const Pad *b,int pressed){
  uint16_t accent = (b->kind==K_Auto)
      ? (!autoRideOn ? P_STEEL : (connectedFlag ? P_GREEN : P_AMBER))
      : b->color;
  uint16_t fill,ink,sub,rim;
  if(b->style==S_Solid){
    fill = pressed?mix565(accent,P_WHITE,175):accent;
    ink  = pressed?accent:P_WHITE; sub=ink;
    rim  = pressed?P_WHITE:mix565(accent,P_WHITE,70);
  } else {
    fill = pressed?accent:P_SURFACE;
    ink  = pressed?inkOn(accent):accent;
    sub  = pressed?inkOn(accent):P_MUTED;
    rim  = pressed?P_WHITE:accent;
  }
  fillRoundRect(b->x,b->y,b->w,b->h,RADIUS,fill);
  drawRoundRect(b->x,b->y,b->w,b->h,RADIUS,rim);
  drawRoundRect(b->x+1,b->y+1,b->w-2,b->h-2,RADIUS-1,rim);

  char autoCap[8]; const char *caption=b->caption;
  if(b->kind==K_Auto){ snprintf(autoCap,sizeof autoCap,"%dS",RIDE_ON_INTERVAL_MS/1000); caption=autoCap; }
  int hasCap = caption[0]!='\0';
  int cx=b->x+b->w/2;
  int cy=b->y+(hasCap?(b->h-14)/2:b->h/2);

  switch(b->glyph){
    case G_Up: case G_Down: case G_Left: case G_Right: drawArrow(b->glyph,cx,cy,17,ink); break;
    case G_Check: drawCheck(cx,cy,ink); break;
    case G_Thumb: drawThumbUp(cx,cy,ink,fill); break;
    case G_Bike:  drawBike(cx,cy,ink); break;
  }
  if(b->label[0]) textCentred(b->label,fitLabel(b->label,b->w-14),cx,cy,ink);
  if(hasCap)      textCentred(caption,&ZwiftSmall11,cx,b->y+b->h-13,sub);
}
static void drawRecess(int x,int y){
  fillRoundRect(x,y,CW,CH,RADIUS,mix565(P_BG,P_SURFACE,110));
  drawRoundRect(x,y,CW,CH,RADIUS,P_LINE);
}
static void drawStatus(int connected){
  fillRect(0,0,W,HEADER_H,P_BG);
  fillTriangle(4,32,11,11,17,11,P_ORANGE);
  fillTriangle(4,32,17,11,10,32,P_ORANGE);
  int x=23;
  textAt("ZWIFT",&ZwiftBold18,x,31,P_ORANGE);
  x+=textWidth("ZWIFT",&ZwiftBold18)+7;
  textAt("REMOTE",&ZwiftBold18,x,31,P_WHITE);
  if(lastWakeMs>0){ char b[16]; snprintf(b,sizeof b,"WAKE %dMS",lastWakeMs);
    textRight(b,&ZwiftSmall11,W-6,30,P_MUTED); }
  const char *state = connected?"CONNECTED":"PAIRING";
  uint16_t col = connected?P_GREEN:P_AMBER;
  int pw = textWidth(state,&ZwiftBold14)+36;
  fillRoundRect(4,46,pw,26,13,P_SURFACE);
  drawRoundRect(4,46,pw,26,13,col);
  fillCircle(19,59,5,col);
  textAt(state,&ZwiftBold14,30,64,col);
  if(autoRideOn){ char b[16]; snprintf(b,sizeof b,"RO %d",rideOnCount);
    textRight(b,&ZwiftBold14,W-6,64,connected?P_GREEN:P_AMBER); }
  drawFastHLine(0,HEADER_H-4,W,P_LINE);
}
static void drawFooter(void){
  drawFastHLine(0,404,W,P_LINE);
  textAt("ROCKER  L/R ARROWS, WORKS SCREEN OFF",&ZwiftSmall11,6,424,P_MUTED);
  textAt("GPIO0   WAKE, TAP=ENTER, HOLD=ESC",&ZwiftSmall11,6,440,P_MUTED);
  if(bondsCleared) textAt("BONDS CLEARED - FORGET DEVICE ON PC",&ZwiftSmall11,6,456,P_AMBER);
  else             textAt("BOTH ROCKERS AT BOOT = CLEAR BONDS",&ZwiftSmall11,6,456,mix565(P_LINE,P_MUTED,120));
}

int main(int argc,char**argv){
  P_BG=rgb(0x0A,0x0E,0x14); P_SURFACE=rgb(0x17,0x1C,0x24); P_LINE=rgb(0x2B,0x33,0x40);
  P_MUTED=rgb(0x7C,0x88,0x99); P_WHITE=rgb(0xFF,0xFF,0xFF);
  P_ORANGE=rgb(0xFC,0x67,0x19); P_BLUE=rgb(0x09,0x96,0xD8); P_GREEN=rgb(0x00,0xA9,0x4F);
  P_AMBER=rgb(0xFF,0xCB,0x0E); P_RED=rgb(0xE4,0x00,0x2B); P_STEEL=rgb(0x6E,0x7D,0x94);

  Pad p[9] = {
    {COL1,ROW0,CW,CH,"","",G_Up,P_BLUE,K_Key,S_Solid},
    {COL0,ROW1,CW,CH,"","",G_Left,P_BLUE,K_Key,S_Solid},
    {COL1,ROW1,CW,CH,"","",G_Check,P_GREEN,K_Key,S_Solid},
    {COL2,ROW1,CW,CH,"","",G_Right,P_BLUE,K_Key,S_Solid},
    {COL1,ROW2,CW,CH,"","",G_Down,P_BLUE,K_Key,S_Solid},
    {COL0,ROW0,CW,CH,"","RIDE ON",G_Thumb,P_ORANGE,K_Key,S_Outline},
    {COL2,ROW0,CW,CH,"AUTO","",G_None,P_STEEL,K_Auto,S_Outline},
    {COL0,BOT_Y,BOT_W,BOT_H,"MENU","ESC",G_None,P_RED,K_Key,S_Outline},
    {COL0+BOT_W+5,BOT_Y,BOT_W,BOT_H,"","GARAGE",G_Bike,P_STEEL,K_Key,S_Outline},
  };
  memcpy(pad,p,sizeof p);

  // argv[1] == "alt" renders the other state: disconnected, AUTO armed, a press held
  int alt = (argc>1 && !strcmp(argv[1],"alt"));
  if(alt){ connectedFlag=0; autoRideOn=1; rideOnCount=12; }

  fillScreen(P_BG);
  drawStatus(connectedFlag);
  drawRecess(COL0,ROW2); drawRecess(COL2,ROW2);
  for(int i=0;i<9;i++) drawPadButton(&pad[i], alt && (i==3||i==6));   // Right held in alt
  drawFooter();

  printf("P6\n%d %d\n255\n",W,H);
  for(int y=0;y<H;y++) for(int x=0;x<W;x++){
    uint16_t c=fb[y][x];
    int r=((c>>11)&0x1F)*255/31, g=((c>>5)&0x3F)*255/63, b=(c&0x1F)*255/31;
    putchar(r); putchar(g); putchar(b);
  }
  return 0;
}
