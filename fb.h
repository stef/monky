#ifndef FB_H
#define FB_H
typedef struct {
   float limit;
   int color;
} color_t;

void init_fb(int fd, int _ttyfd);
void init_ft(char* ttfp, size_t ttfp_len);
void display(char *str, size_t str_len, float* data, uint8_t idx, size_t data_len, int fg, int bg, color_t *colors);
void newline(int start, int y, int bg);
#endif
