#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

// todo do s6 notifywhenup protocol

#define GROUPID 65534
#define USERID 65534

#define MAX_ENT 3000
#define MAX_DSK 100*1024
#define MAX_UP 400
#define MAX_DOWN 1024
#define MAX_TMP 95

#define SAMPLES 16

#if __x86_64__
#define UD "ld"
#else
#define UD "lld"
#endif
const char **colors;
const char *ansi_colors[]={
  "\x1b[38;5;8m%c\x1b[38;5;7m",
  "\x1b[38;5;8m%c\x1b[38;5;7m",
  "\x1b[38;5;4m%c\x1b[38;5;7m",
  "\x1b[38;5;12m%c\x1b[38;5;7m",
  "\x1b[38;5;5m%c\x1b[38;5;7m",
  "\x1b[38;5;13m%c\x1b[38;5;7m",
  "\x1b[38;5;1m%c\x1b[38;5;7m",
  "\x1b[38;5;9m%c\x1b[38;5;7m",
  "\x1b[38;5;10m%c\x1b[38;5;7m",
  "\x1b[38;5;2m%c\x1b[38;5;7m",
  "\x1b[38;5;6m%c\x1b[38;5;7m",
  "\x1b[38;5;14m%c\x1b[38;5;7m",
  "\x1b[38;5;3m%c\x1b[38;5;7m",
  "\x1b[38;5;11m%c\x1b[38;5;7m",
  "\x1b[38;5;7m%c\x1b[38;5;7m",
  "\x1b[38;5;15m%c\x1b[38;5;7m"};
const char *tmux_colors[]={
  "#[fg=colour8]%c#[fg=white]",
  "#[fg=colour8]%c#[fg=white]",
  "#[fg=colour4]%c#[fg=white]",
  "#[fg=colour12]%c#[fg=white]",
  "#[fg=colour5]%c#[fg=white]",
  "#[fg=colour13]%c#[fg=white]",
  "#[fg=colour1]%c#[fg=white]",
  "#[fg=colour9]%c#[fg=white]",
  "#[fg=colour10]%c#[fg=white]",
  "#[fg=colour2]%c#[fg=white]",
  "#[fg=colour6]%c#[fg=white]",
  "#[fg=colour14]%c#[fg=white]",
  "#[fg=colour3]%c#[fg=white]",
  "#[fg=colour11]%c#[fg=white]",
  "#[fg=colour7]%c#[fg=white]",
  "#[fg=colour15]%c#[fg=white]"};

static void push(float q[], uint8_t* idx, float val) {
  *idx = (*idx+1) % SAMPLES;
  q[*idx]=val;
}

#define HGRAM_WIDTH 4
static char* hgram(char* bar, const size_t hgsize, const float q[], const uint8_t idx, const float max, const float min) {
  const char ticks[]={'0', '1', '2', '3', '4', '5', '6', '7',
                      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  uint8_t i,j, oidx;
  char hg_idx=1;
  float avg, val, scale;
  scale = (max-min)/16;
  if(scale==0) scale=1;
  for(i=HGRAM_WIDTH;i>0;i--) {
    avg=0;
    for(j=0;j<(SAMPLES/HGRAM_WIDTH);j++) {
      val=q[(((i-1)*(SAMPLES/HGRAM_WIDTH))+j+idx)%SAMPLES];
      if(val>max) {
        val=max;
      }
      avg+=val;
    }
    avg/=(SAMPLES/HGRAM_WIDTH);
    oidx = (int)((avg - min)/scale);
    hg_idx+=snprintf(bar+hg_idx-1, hgsize-hg_idx, colors[oidx], ticks[oidx]);
  }
  return bar;
}

float cpu_samples[SAMPLES];
uint8_t cpu_samples_idx=0;
uint64_t prevcputotal, prevcpuunused;
float cpu(void) {
  uint32_t user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
  uint64_t total, unused;
  float dtotal, dunused, ret;
  char label[5];
  FILE* fd;
  fd=fopen("/proc/stat","r");
  fscanf(fd,"%4s %d %d %d %d %d %d %d %d %d %d", label, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
  fclose(fd);
  if(memcmp("cpu",label,4)!=0) {
    printf("err: /proc/stat does not start with cpu line\n");
    exit(1);
  }
  total = user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;
  unused = idle + iowait;

  // get previous sample
  if(prevcputotal == 0) {
    prevcputotal = total;
  }
  if(prevcpuunused == 0) {
    prevcpuunused = unused;
  }
  dtotal=total-prevcputotal;
  dunused=unused-prevcpuunused;

  // remember current sample
  prevcputotal = total;
  prevcpuunused = unused;
  ret=(100.0*( dtotal - dunused )) / dtotal;
  push(cpu_samples, &cpu_samples_idx, ret);
  return ret;
}

float ent_samples[SAMPLES];
uint8_t ent_samples_idx=0;
uint32_t ent(void) {
  uint32_t avail=0;
  FILE* fd;
  fd=fopen("/proc/sys/kernel/random/entropy_avail","r");
  fscanf(fd,"%d\n", &avail);
  fclose(fd);
  push(ent_samples, &ent_samples_idx, avail);
  return avail;
}

FILE *tempfd;
float temp_samples[SAMPLES];
uint8_t temp_samples_idx=0;
uint32_t temp(void) {
  uint32_t t=0;
  fseek(tempfd,0,0);
  fscanf(tempfd,"%d\n", &t);
  push(temp_samples, &temp_samples_idx, t/1000);
  return t/1000;
}

#define MEM_LABEL_LEN 14 // length of MemAvailable: with some leeway
float mem_samples[SAMPLES];
uint8_t mem_samples_idx=0;
float mem(void) {
  float max=0, cur=0, ret;
  uint64_t val;
  char label[MEM_LABEL_LEN];
  FILE* fd;
  fd=fopen("/proc/meminfo","r");
  while(max==0 || cur==0) {
    if(fscanf(fd,"%14s %"UD" %*s\n", label, &val)!=2) {
      break;
    }
    if(memcmp("MemTotal:",label,10)==0) {
      max=(float) val;
    } else if(memcmp("MemAvailable:",label,MEM_LABEL_LEN)==0) {
      cur=(float) val;
    }
  }
  fclose(fd);
  if(max==0 || cur==0) {
    printf("err: could not measure mem\n");
    exit(1);
  }
  ret=100.0 - ((cur*100) / max);
  push(mem_samples, &mem_samples_idx, ret);
  return ret;
}

FILE* batfd;
float bat_samples[SAMPLES];
uint8_t bat_samples_idx=0;
float bat(char *status) {
  float max=0, cur=0, ret;
  char val[32];
  char label[128];
  *status=0;
  fseek(batfd,0,0);
  while(max==0 || cur==0 || *status==0) {
    if(fscanf(batfd,"%128[A-Z_]=%32s\n", label, val)!=2) {
      printf("%s || %s\n", label, val);
      continue;
    }
    if(memcmp("POWER_SUPPLY_STATUS:",label,19)==0) {
      if(memcmp(val,"Discharging",11)==0) {
        *status='D';
      } else if(memcmp(val,"Charging",8)==0) {
        *status='D';
      } else if(memcmp(val,"Unknown",7)==0) {
        *status='U';
      } else {
        *status='?';
      }
    } else if(memcmp("POWER_SUPPLY_ENERGY_FULL",label,23)==0) {
      max=atof(val);
    } else if(memcmp("POWER_SUPPLY_ENERGY_NOW",label,22)==0) {
      cur=atof(val);
    }
  }
  if(max==0 || cur==0 || *status==0) {
    printf("err: could not measure battery\n");
    exit(1);
  }
  ret=((cur*100) / max);
  push(bat_samples, &bat_samples_idx, ret);
  return ret;
}

uint64_t previo;
float dsk_samples[SAMPLES];
uint8_t dsk_samples_idx=0;
uint64_t dsk(char* dev) {
  uint64_t reads, writes, total, ret;
  char label[10];
  FILE* fd;
  fd=fopen("/proc/diskstats","r");
  while(memcmp(label, dev, strlen(dev)+1)!=0) {
      if(fscanf(fd,"%*d %*d %10s %*d %*d %"UD" %*d %*d %*d %"UD" %*d %*d %*d %*d", label, &reads, &writes)!=3) break;
  }
  fclose(fd);
  if(memcmp(dev,label,strlen(dev)+1)!=0) {
    printf("err: couldn't find %s in /proc/diskstat\n", dev);
    exit(1);
  }
  total = reads+writes;

  // get previous sample
  if(previo == 0) {
    previo = total;
  }

  // remember current sample
  ret=total-previo;
  previo = total;
  push(dsk_samples, &dsk_samples_idx, ret);
  return ret;
}

uint64_t pup, pdown;
float up_samples[SAMPLES], down_samples[SAMPLES];
uint8_t up_samples_idx=0, down_samples_idx=0;
void net(char* dev, float *up, float *down) {
  uint64_t read, write;
  char label[10];
  FILE* fd;

  fd=fopen("/proc/net/dev","r");
  while(memcmp(label, dev, strlen(dev)+1)!=0) {
    if(fscanf(fd," %10s %"UD" %*d %*d %*d %*d %*d %*d %*d %"UD" %*d %*d %*d %*d %*d %*d %*d\n", label, &read, &write)==EOF) {
      break;
    }
  }
  fclose(fd);
  if(memcmp(dev,label,strlen(dev)+1)!=0) {
    printf("err: couldn't find %s in /proc/net/dev\n", dev);
    exit(1);
  }

  // get previous sample
  if(pup == 0) {
    pup = write;
  }
  if(pdown == 0) {
    pdown = read;
  }

  *up=((float)(write - pup)) / 1024.0;
  *down=((float)(read - pdown)) / 1024.0;

  push(up_samples, &up_samples_idx, *up);
  push(down_samples, &down_samples_idx, *down);

  // remember current sample
  pup=write;
  pdown=read;
}

int main(int argc, char** argv) {
  float up, down;
  char upbar[128], downbar[128], cpubar[128], entbar[128],
       membar[128], dskbar[128],  batbar[128], tempbar[128], status, *terminator;
  FILE *out;
  if(argc<3) {
    printf("usage: %s diskdev netdev: [tmuxoutfile]\n", argv[0]);
    exit(1);
  }

  // open all files
  tempfd=fopen("/sys/class/thermal/thermal_zone0/temp","r");
  batfd=fopen("/sys/class/power_supply/BAT0/uevent","r");

  // drop privileges
  if (geteuid() == 0) {
    /* process is running as root, drop privileges */
    if (setgid(GROUPID) != 0) {
      printf("setgid: Unable to drop group privileges: %s", strerror(errno));
      exit(1);
    }
    if (setuid(USERID) != 0) {
      printf("setuid: Unable to drop user privileges: %s", strerror(errno));
      exit(1);
    }
  }
  if (setuid(0) != -1 && geteuid() == 0) {
    printf("ERROR: Managed to regain root privileges?");
    exit(1);
  }

  // optionally open output file for tmux
  if(argc>3) {
    out = fopen(argv[3], "w");
    colors=tmux_colors;
    terminator="\n";
  } else {
    out = stdout;
    colors=ansi_colors;
    terminator="                     \r";
  }

  // todo also do seccomp lockdown
  // 01-22 03:08 -O> http://www.insanitybit.com/2014/09/08/3719/
  // https://lwn.net/Articles/491308/
  // https://s3hh.wordpress.com/2012/07/24/playing-with-seccomp/
  // https://wiki.tizen.org/wiki/Security:Seccomp
  // http://sourceforge.net/p/libseccomp/mailman/libseccomp-discuss/thread/4892415.SaF4mnePOG@sifl/

  // run mainloop
  while(1) {
    net(argv[2], &up, &down);
    if(out!=stdout) {
      ftruncate(fileno(out), 0);
      fseek(out,0,0);
    }
    fprintf(out, "e:%s %4d c:%s %5.1f%% m:%s %5.1f%% d:%s %5"UD"kB/s u:%s %5.1fkB/s d:%s %5.1fkB/s t:%s %2dC b:%s %5.1f%%:%c%s",
            hgram(entbar, 128, ent_samples, ent_samples_idx, (float) MAX_ENT, (float) 0),
            ent(),
            hgram(cpubar, 128, cpu_samples, cpu_samples_idx, (float) 100, (float) 0),
            cpu(),
            hgram(membar, 128, mem_samples, mem_samples_idx, (float) 100, (float) 0),
            mem(),
            hgram(dskbar, 128, dsk_samples, dsk_samples_idx, (float) MAX_DSK, (float) 0),
            dsk(argv[1]),
            hgram(upbar, 128, up_samples, up_samples_idx, (float) MAX_UP, (float) 0),
            up,
            hgram(downbar, 128, down_samples, down_samples_idx, (float) MAX_DOWN, (float) 0),
            down,
            hgram(tempbar, 128, temp_samples, temp_samples_idx, (float) MAX_TMP, (float) 0),
            temp(),
            hgram(batbar, 128, bat_samples, bat_samples_idx, (float) 100, (float) 0),
            bat(&status),
            status,
            terminator
            );
    fflush(out);
    sleep(1);
  }
  return 0;
}
