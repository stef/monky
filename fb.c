// standalone compile with: gcc -g -DSTANDALONE_FB -lfreetype -I/usr/include/freetype2 -o fb fb.c
#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <linux/vt.h>
#include "fb.h"

#define MAX_GLYPHS 170

// globals

int mainvt=-1;

int direction=0;

// fb state/info
int fbfd = -1;
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
long int screensize = 0;
char *fbp = NULL;

int ttyfd;

// freetype state/info
FT_Library library;
FT_Face face;

int getvt() {
   if(ttyfd==-1) {
      printf("couldn't open /dev/tty0\n");
      exit(1);
   }
   struct vt_stat vtstat;
   if (ioctl(ttyfd, VT_GETSTATE, &vtstat) < 0) {
      return -1;
   }
   return vtstat.v_active;
}

void init_fb(int fd, int _ttyfd) {
  fbfd = fd;
  if (fbfd == -1) {
    perror("Error: cannot open framebuffer device");
    exit(1);
  }
  //printf("The framebuffer device was opened successfully.\n");

  // Get fixed screen information
  if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) {
    perror("Error reading fixed information");
    exit(2);
  }

  // Get variable screen information
  if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
    perror("Error reading variable information");
    exit(3);
  }

  //printf("%dx%d, %dbpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

  // Figure out the size of the screen in bytes
  screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;

  // Map the device to memory
  fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fbfd, 0);
  if ((long int)fbp == -1) {
    perror("Error: failed to map framebuffer device to memory");
    exit(4);
  }
  //printf("The framebuffer device was mapped to memory successfully.\n");

  //remember the current vt
  ttyfd=_ttyfd;
  if((mainvt=getvt())==-1) {
     printf("unable to determine current vt\n");
     exit(1);
  };
}

void init_ft(char* ttfp, size_t ttfp_len) {
   int error;

   //printf("Initializing Freetype2\n");
   error = FT_Init_FreeType( &library );
   if ( error ) return;

   //printf("Loading font: %s\n", font);
   FT_Open_Args args;
   args.flags=FT_OPEN_MEMORY;
   args.memory_base=(unsigned char*) ttfp;
   args.memory_size=ttfp_len;
   error = FT_Open_Face( library, &args, 0, &face );
   if ( error == FT_Err_Unknown_File_Format ) {
      printf("unknown font format\n");
      exit(1);
   } else if ( error ) {
      printf("font error: %d\n", error);
      exit(1);
   }

   error = FT_Set_Char_Size( face,    /* handle to face object           */
         0,       /* char_width in 1/64th of points  */
         90,      /* char_height in 1/64th of points */
         600,     /* horizontal device resolution    */
         600 );   /* vertical device resolution      */
   if ( error ) {
      printf("font size error: %d\n", error);
      exit(1);
   }
}

void compute_string_bbox(FT_Glyph *glyphs, FT_UInt num_glyphs, FT_Vector *pos,  FT_BBox *bbox ) {
  // could be so much more efficient with monospaced fonts.
  FT_BBox  glyph_bbox;
  int n;

  /* initialize string bbox to "empty" values */
  bbox->xMin = bbox->yMin =  32000;
  bbox->xMax = bbox->yMax = -32000;

  /* for each glyph image, compute its bounding box, */
  /* translate it, and grow the string bbox          */
  for ( n = 0; n < num_glyphs; n++ ) {
    FT_Glyph_Get_CBox( glyphs[n], ft_glyph_bbox_pixels,
                       &glyph_bbox );

    glyph_bbox.xMin += pos[n].x;
    glyph_bbox.xMax += pos[n].x;
    glyph_bbox.yMin += pos[n].y;
    glyph_bbox.yMax += pos[n].y;

    if ( glyph_bbox.xMin < bbox->xMin )
      bbox->xMin = glyph_bbox.xMin;

    if ( glyph_bbox.yMin < bbox->yMin )
      bbox->yMin = glyph_bbox.yMin;

    if ( glyph_bbox.xMax > bbox->xMax )
      bbox->xMax = glyph_bbox.xMax;

    if ( glyph_bbox.yMax > bbox->yMax )
      bbox->yMax = glyph_bbox.yMax;
  }
  /* check that we really grew the string bbox */
  if ( bbox->xMin > bbox->xMax ) {
    bbox->xMin = 0;
    bbox->yMin = 0;
    bbox->xMax = 0;
    bbox->yMax = 0;
  }
}

int draw_glyph(FT_Bitmap *bm, int x,int y, int bgcol) {
  long int location = 0, row, col;
  if(bm==NULL || fbp==NULL || x<0 || y<0 || x+bm->width>vinfo.xres || y+bm->rows>vinfo.yres) {
     return -1;
  }
  for (row=bm->rows;row>0;row--) {
     for (col=0;col<bm->width;col++) {
        location = (x+col+vinfo.xoffset) * (vinfo.bits_per_pixel/8) +
           (y+(row-1)+vinfo.yoffset) * finfo.line_length;
        if (vinfo.bits_per_pixel == 32) {
           if(bm->pixel_mode==FT_PIXEL_MODE_GRAY) {
              if(bm->buffer[((row-1)*bm->width)+col]!=0) {
                 *(fbp+location)=bm->buffer[((row-1)*bm->width)+col];
                 *(fbp+location+1)=bm->buffer[((row-1)*bm->width)+col];
                 *(fbp+location+2)=bm->buffer[((row-1)*bm->width)+col];
                 *(fbp+location+3)=0xff;
              } else {
                 *((int*)(fbp+location))=bgcol;
              }
           } //else if(bm->pixel_mode==FT_PIXEL_MODE_MONO) {
           //}
        }// else  { //assume 16bpp
        //}
     }
  }
  return 0;
}


int fill(int x, int y, int w, int h, int col) {
   if(fbp==NULL || x<0 || y<0 || x+w>vinfo.xres || y+h>vinfo.yres) {
      return -1;
   }
   long int location = 0;
   int n,m;
   for(n=0; n<h;n++) {
      location = (x+vinfo.xoffset) * (vinfo.bits_per_pixel/8) +
         (y+n+vinfo.yoffset) * finfo.line_length;
      for(m=0; m<w;m++) {
         void* tmp=fbp+location+(m*(vinfo.bits_per_pixel/8));
         *((int*)tmp)=col;
      }
   }
   return 0;
}
int print(char* txt, int x, int y, int bgcol) {
   FT_Glyph      glyphs[MAX_GLYPHS];   /* glyph image    */
   FT_Vector     pos   [MAX_GLYPHS];   /* glyph position */
   FT_GlyphSlot  slot;  /* a small shortcut */
   FT_UInt       glyph_index;
   FT_UInt       num_glyphs;
   FT_BBox       string_bbox;
   int           pen_x, pen_y;
   int           string_width, string_height;
   int           n, error, num_chars=strlen(txt);

   slot = face->glyph;                /* a small shortcut */

   pen_x = 0;   /* start at (0,0) */
   pen_y = 0;

   num_glyphs = 0;

   for ( n = 0; n < num_chars; n++ ) {
      /* convert character code to glyph index */
      glyph_index = FT_Get_Char_Index( face, txt[n] );
      if(glyph_index<0) {
         return -1;
      }
      /* store current pen position */
      pos[num_glyphs].x = pen_x;
      pos[num_glyphs].y = pen_y;

      /* load glyph image into the slot without rendering */
      error = FT_Load_Glyph( face, glyph_index, FT_LOAD_DEFAULT );
      if ( error )
         continue;  /* ignore errors, jump to next glyph */

      /* extract glyph image and store it in our table */
      error = FT_Get_Glyph( face->glyph, &glyphs[num_glyphs] );
      if ( error )
         continue;  /* ignore errors, jump to next glyph */

      /* increment pen position */
      pen_x += slot->advance.x >> 6;

      /* increment number of glyphs */
      num_glyphs++;
   }

   compute_string_bbox(glyphs, num_chars, pos,  &string_bbox);
   /* compute string dimensions in integer pixels */
   string_width  = string_bbox.xMax - string_bbox.xMin;
   string_height = string_bbox.yMax - string_bbox.yMin;

   if(direction==-1) x = x - (pos[num_glyphs-1].x + 9);

   if(fill(x,y,string_width+3,string_height+4,bgcol)==-1) {
      printf("failed to fill area: x=%d y=%d w=%d h=%d\n",x,y,string_width+3,string_height+4);
      exit(1);
   }

   // compute start pen position
   y += string_height; // bug if _ then height is 12, otherwise less... :/

   for(n = 0; n < num_glyphs; n++) {
      FT_Glyph   image;
      FT_Vector  pen;
      image = glyphs[n];
      pen.x = pos[n].x;
      pen.y = pos[n].y;

      error = FT_Glyph_To_Bitmap( &image, FT_RENDER_MODE_NORMAL, &pen, 0 );
      if ( error ) {
         printf("font rendering error: %d\n",error);
         exit(1);
      }
      FT_BitmapGlyph bit = (FT_BitmapGlyph)image;
      draw_glyph( &bit->bitmap,
                   x+pen.x+1, // + bit->left,
                   y - (bit->top-1),
                   bgcol);
      FT_Done_Glyph( image );
   }
   for(n = 0; n < num_glyphs; n++)
      FT_Done_Glyph( glyphs[n] );
   if(direction==-1) return x;
   return x + pos[num_glyphs-1].x + 9;
}

int histogram(float *data, uint8_t idx, size_t len, int x, int y, int h, color_t *colors, int bg) {
   if(direction!=1 && direction!=-1) {
      return -1;
   }
   float max=-INFINITY, min=INFINITY, range;
   unsigned int i, j, x1, y1, location, fg=colors[0].color;
   //find min/max
   for(i=0;i<len;i++) {
      if(data[(i+idx) % len]<min) min=data[(i+idx) % len];
      if(data[(i+idx) % len]>max) max=data[(i+idx) % len];
   }

   for(i=0;data[idx]>=colors[i].limit && !isnan(colors[i].limit);i++) {
      fg=colors[i].color;
   }

   range=max-min;
   if(direction==-1) x-=len+1;
   for(i=0;i<len;i++) {
      x1=x+i;
      // draw lower line
      location = (x1+vinfo.xoffset) * (vinfo.bits_per_pixel/8) +
         (y+h+vinfo.yoffset) * finfo.line_length;
      *((int*)(fbp+location))=fg;
      // draw bar
      for(j=0;j<h;j++) {
         y1=(y+(h-1))-j;
         location = (x1+vinfo.xoffset) * (vinfo.bits_per_pixel/8) +
            (y1+vinfo.yoffset) * finfo.line_length;
         if (vinfo.bits_per_pixel == 32) {
            if(j<=(int)(((data[(i+idx) % len]-min)*h)/range))
               *((int*)(fbp+location))=fg;
            else
               *((int*)(fbp+location))=bg;
         } // else assume 16bit
      // draw upper line
      location = (x1+vinfo.xoffset) * (vinfo.bits_per_pixel/8) +
         (y+vinfo.yoffset) * finfo.line_length;
      *((int*)(fbp+location))=fg;
      }
   }
   if(direction==1) {
      return x+len+1;
   }
   // right aligned
   return x;
}

int xd=0, yd;
void newline(int start, int y, int bg) {
  if(start==1) {
     if(xd!=0 && xd!=vinfo.xres) fill(0,y,xd+2,12, bg);
     xd=0;
     direction=1;
  } else if(start==-1) {
     if(xd!=vinfo.xres && xd!=0) fill(xd-2,y,vinfo.xres-xd,12, bg);
     xd=vinfo.xres;
     direction=-1;
  } else return;
  yd=y;
}

void display(char *str, size_t str_len,
             float* data, uint8_t idx, size_t data_len,
             int fg, int bg, color_t *colors) {
  int ret;
  if(mainvt!=getvt()) return;
  if(direction==-1) {
     ret=histogram(data, idx, data_len, xd, yd, 10, colors, bg);
     if(ret!=-1) xd=print(str, ret, yd, bg);
  } else if(direction==1) {
     xd=print(str, xd, yd, bg);
     ret=histogram(data, idx, data_len, xd, yd, 10, colors, bg);
     if(ret!=-1) xd=ret;
  }
}

#ifdef STANDALONE_FB
int get(void* dest,const int x,const int y,const int w,const int h) {
  long location = 0;
  long int i = 0;
  int y1;
  if(fbp==NULL || x<0 || y<0 || x+w>vinfo.xres || y+h>vinfo.yres) {
     return -1;
  }
  for (y1=y;y1<y+h;y1++) {
    location = (x+vinfo.xoffset) * (vinfo.bits_per_pixel/8) +
      (y1+vinfo.yoffset) * finfo.line_length;

    if (vinfo.bits_per_pixel == 32) {
      memcpy(dest+i,(void*) (fbp+location),w*4);
      i+=w*4;
    } else  { //assume 16bpp
      memcpy(dest+i,(void*) (fbp+location),w*2);
      i+=w*2;
    }
  }
  return 0;
}

void screenshot(char* fn) {
  char buff[screensize];
  get((void*) buff,0,0,vinfo.xres,vinfo.yres);
  FILE *out = fopen(fn,"w");
  fprintf(out, "P6\n%d %d\n255\n", vinfo.xres,vinfo.yres);
  int i;
  for(i=0;i<vinfo.xres*vinfo.yres;i++) {
    fprintf(out,"%c%c%c",buff[(i*4)+2], buff[(i*4)+1], buff[(i*4)]);
  }
  fclose(out);
}

void close_fb() { // never called by monky
  if(fbp!=NULL) {
     munmap(fbp, screensize);
     fbp=NULL;
  }
  if(fbfd>=0) {
     close(fbfd);
     fbfd=-1;
  }
}

int main() {
  init_fb();

  float data[]={1,1,1,2,2,3,3,5,7,6,5,3,2,1,1,1,4,3,1,1,1,2,1,2,1,2};
  display("hello world", 11, data, 20, 0x00ff0000, 0x00333333, 1);
  int n;
  for(n=0;n<9;n++) {
     display("hello world", 11, data, 20, 0x00ff0000, 0x00333333, 0);
  }
  screenshot("fbft.ppm");
  close_fb();
  return 0;
}
#endif //STANDALONE_FB
