#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <seccomp.h>   /* libseccomp */
#include <sys/prctl.h> /* prctl */
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>

// set uid/guid to monky/readproc
#define USERID 113
#define GROUPID 30
// set uid/guid to nobody
//#define USERID 65534
//#define GROUPID 65534

// max values for hgram
#define MAX_ENT 3000       // max entropy of system
#define MAX_DSK 100*1024   // max speed of disk (in kB/s)
#define MAX_UP 400         // max net uplink speed (in kB/s)
#define MAX_DOWN 5*1024    // max net downlink speed (in kB/s)
#define MAX_TMP 95         // max cpu temp

// amount of history to keep
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

FILE *out;
char tbar[128];

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

FILE* cpufd;
float cpu_samples[SAMPLES];
uint8_t cpu_samples_idx=0;
uint64_t prevcputotal, prevcpuunused;
void cpu(void) {
  uint32_t user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
  uint64_t total, unused;
  float dtotal, dunused, ret;
  char label[5];
  fseek(cpufd,0,SEEK_SET);
  fscanf(cpufd,"%4s %d %d %d %d %d %d %d %d %d %d",
         label, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
  while(fgetc(cpufd)!=EOF); // weird way of flushing /proc files
  if(strncmp("cpu",label,sizeof(label))!=0) {
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
  fprintf(out, "c:%s %5.1f%% ", hgram(tbar, 128, cpu_samples, cpu_samples_idx, (float) 100, (float) 0), ret);
}

FILE* entfd;
float ent_samples[SAMPLES];
uint8_t ent_samples_idx=0;
void ent(void) {
  uint32_t avail=0;
  fseek(entfd,0,SEEK_SET);
  fscanf(entfd,"%d\n", &avail);
  push(ent_samples, &ent_samples_idx, (float) avail);
  fprintf(out, "e:%s %4d ", hgram(tbar, 128, ent_samples, ent_samples_idx, (float) MAX_ENT, (float) 0), avail);
}

FILE *tempfd;
float temp_samples[SAMPLES];
uint8_t temp_samples_idx=0;
void temp(void) {
  uint32_t t=0;
  fseek(tempfd,0,0);
  fscanf(tempfd,"%d\n", &t);
  push(temp_samples, &temp_samples_idx, ((float)t)/1000.0);
  fprintf(out, "t:%s %2dC ", hgram(tbar, 128, temp_samples, temp_samples_idx, (float) MAX_TMP, (float) 0), t/1000);
}

FILE* memfd;
float mem_samples[SAMPLES];
uint8_t mem_samples_idx=0;
void mem(void) {
  float max=0, cur=0, ret;
  uint64_t val;
  char label[14];
  fseek(memfd,0,SEEK_SET);
  while(max==0 || cur==0) {
    if(fscanf(memfd,"%14s %"UD" %*s\n", label, &val)!=2) {
      break;
    }
    if(strncmp("MemTotal:",label,sizeof(label))==0) {
      max=(float) val;
    } else if(strncmp("MemAvailable:",label,sizeof(label))==0) {
      cur=(float) val;
    }
  }
  while(fgetc(memfd)!=EOF); // weird way of flushing /proc files
  if(max==0 || cur==0) {
    printf("err: could not measure mem\n");
    exit(1);
  }
  ret=100.0 - ((cur*100) / max);
  push(mem_samples, &mem_samples_idx, ret);
  fprintf(out, "m:%s %5.1f%% ", hgram(tbar, 128, mem_samples, mem_samples_idx, (float) 100, (float) 0), ret);
}

FILE* batfd;
float bat_samples[SAMPLES];
uint8_t bat_samples_idx=0;
void bat() {
  float max=0, cur=0, ret;
  char val[32], status;
  char label[128];
  status=0;
  fseek(batfd,0,0);
  while(max==0 || cur==0 || status==0) {
    if(fscanf(batfd,"%128[A-Z_]=%32s\n", label, val)!=2) {
      printf("%s || %s\n", label, val);
      continue;
    }
    if(strncmp("POWER_SUPPLY_STATUS",label,sizeof(label))==0) {
      if(strncmp(val,"Discharging",sizeof(val))==0) {
        status='D';
      } else if(strncmp(val,"Charging",sizeof(val))==0) {
        status='D';
      } else if(strncmp(val,"Unknown",sizeof(val))==0) {
        status='U';
      } else {
        status='?';
      }
    } else if(strncmp("POWER_SUPPLY_ENERGY_FULL",label,sizeof(label))==0) {
      max=atof(val);
    } else if(strncmp("POWER_SUPPLY_ENERGY_NOW",label,sizeof(label))==0) {
      cur=atof(val);
    }
  }
  while(fgetc(batfd)!=EOF); // weird way of flushing /proc files
  if(max==0 || cur==0 || status==0) {
    printf("err: could not measure battery\n");
    exit(1);
  }
  ret=((cur*100) / max);
  push(bat_samples, &bat_samples_idx, ret);
  fprintf(out, "b:%s %5.1f%%:%c ", hgram(tbar, 128, bat_samples, bat_samples_idx, (float) 100, (float) 0), ret, status);
}

FILE* dskfd;
uint64_t previo;
float dsk_samples[SAMPLES];
uint8_t dsk_samples_idx=0;
void dsk(char* dev) {
  uint64_t reads, writes, total, ret;
  char label[10];
  fseek(dskfd,0,0);
  while(strncmp(label, dev, sizeof(label))!=0) {
      if(fscanf(dskfd,"%*d %*d %10s %*d %*d %"UD" %*d %*d %*d %"UD" %*d %*d %*d %*d", label, &reads, &writes)!=3) break;
  }
  while(fgetc(dskfd)!=EOF); // weird way of flushing /proc files
  if(strncmp(dev,label,sizeof(label))!=0) {
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
  fprintf(out, "d:%s %5"UD"kB/s ", hgram(tbar, 128, dsk_samples, dsk_samples_idx, (float) MAX_DSK, (float) 0), ret);
}

FILE* netfd;
uint64_t pup, pdown;
float up_samples[SAMPLES], down_samples[SAMPLES];
uint8_t up_samples_idx=0, down_samples_idx=0;
void net(char* dev) {
  float up, down;
  uint64_t read=0, write=0;
  char label[10]="";

  fseek(netfd,0,SEEK_SET);
  while(strncmp(label, dev, sizeof(label))!=0) {
    if(fscanf(netfd," %10s %"UD" %*d %*d %*d %*d %*d %*d %*d %"UD" %*d %*d %*d %*d %*d %*d %*d\n", label, &read, &write)==EOF) {
      break;
    }
  }
  while(fgetc(netfd)!=EOF); // weird way of flushing /proc files
  if(strncmp(dev,label,sizeof(label))!=0) {
    // device does not exist, push 0 into history and return
    push(up_samples, &up_samples_idx, 0);
    push(down_samples, &down_samples_idx, 0);
    return;
  }

  // get previous sample
  if(pup == 0) {
    pup = write;
  }
  if(pdown == 0) {
    pdown = read;
  }

  up=((float)(write - pup)) / 1024.0;
  down=((float)(read - pdown)) / 1024.0;

  push(up_samples, &up_samples_idx, up);
  push(down_samples, &down_samples_idx, down);

  // remember current sample
  pup=write;
  pdown=read;
  fprintf(out, "u:%s %5.1fkB/s ", hgram(tbar, 128, up_samples, up_samples_idx, (float) MAX_UP, (float) 0), up);
  fprintf(out, "d:%s %5.1fkB/s ", hgram(tbar, 128, down_samples, down_samples_idx, (float) MAX_DOWN, (float) 0), down);
}

void lock_seccomp(int fds[], size_t fdsize, int out) {
  // Init the filter
  int i;
  scmp_filter_ctx ctx;
  ctx = seccomp_init(SCMP_ACT_KILL); // default action: kill

#if __x86_64__
  //readv([3,4,5,6,7,8,9]
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readv), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));
  //lseek([3,4,5,6,7,8,9], 0, [0], SEEK_SET
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));
  //lseek(out
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 1,
                        SCMP_A0(SCMP_CMP_EQ, out));
  //ioctl(out
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
                        SCMP_A0(SCMP_CMP_EQ, out));
  //write(out
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(writev), 1,
                        SCMP_A0(SCMP_CMP_EQ, out));
#else //__x86_64__
  //rt_sigprocmask(SIG_SETMASK, [], NULL, 8) = 0
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigprocmask), 3,
                        SCMP_A0(SCMP_CMP_EQ, SIG_SETMASK),
                        SCMP_A2(SCMP_CMP_EQ, NULL),
                        SCMP_A3(SCMP_CMP_EQ, 8));
  //rt_sigprocmask(SIG_BLOCK, [CHLD], [], 8) = 0
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigprocmask), 2,
                        SCMP_A0(SCMP_CMP_EQ, SIG_BLOCK),
                        SCMP_A3(SCMP_CMP_EQ, 8));
  //rt_sigaction(SIGCHLD, NULL, {SIG_DFL, [], 0}, 8) = 0
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigaction), 3,
                        SCMP_A0(SCMP_CMP_EQ, SIGCHLD),
                        SCMP_A1(SCMP_CMP_EQ, NULL),
                        SCMP_A3(SCMP_CMP_EQ, 8));

  //mmap2(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap2), 0);

  // brk
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(brk), 0);

  //read([3,4,5,6,7,8,9]
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(read), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));

  //_llseek([3,4,5,6,7,8,9], 0, [0], SEEK_SET
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(_llseek), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(_llseek), 1,
                        SCMP_A0(SCMP_CMP_EQ, out));

  //fstat64([3,4,5,6,7,8,9], {st_mode=S_IFREG|0444, st_size=4096, ...}) = 0
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fstat64), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));
  //fstat(out
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fstat64), 1,
                        SCMP_A0(SCMP_CMP_EQ, out));

  //write(1
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(write), 1,
                        SCMP_A0(SCMP_CMP_EQ, out));
#endif
  //ftruncate(out
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ftruncate), 1,
                        SCMP_A0(SCMP_CMP_EQ, out));
  //nanosleep({1, 0}
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(nanosleep), 0);

  // enable seccomp rules
  seccomp_load(ctx);
}

void drop_privs(void) {
  if(geteuid()==0) {
    // process is running as root, drop privileges
    if(setgid(GROUPID)!=0) {
      printf("setgid: Unable to drop group privileges: %s", strerror(errno));
      exit(1);
    }
    if(setuid(USERID)!=0) {
      printf("setuid: Unable to drop user privileges: %s", strerror(errno));
      exit(1);
    }
  }
  if(setuid(0)!=-1 && geteuid()==0) {
    printf("ERROR: Managed to regain root privileges?");
    exit(1);
  }
}

int main(int argc, char** argv) {
  char *terminator;
  if(argc<3) {
    printf("usage: %s diskdev netdev: [tmuxoutfile]\n", argv[0]);
    exit(1);
  }

  // do not gain new privs
  prctl(PR_SET_NO_NEW_PRIVS, 1);
  // disable ptrace
  prctl(PR_SET_DUMPABLE, 0);

  // open all files
  tempfd=fopen("/sys/class/thermal/thermal_zone0/temp","r");
  batfd=fopen("/sys/class/power_supply/BAT0/uevent","r");
  cpufd=fopen("/proc/stat","r");
  memfd=fopen("/proc/meminfo","r");
  dskfd=fopen("/proc/diskstats","r");
  netfd=fopen("/proc/net/dev","r");
  entfd=fopen("/proc/sys/kernel/random/entropy_avail","r");

  // drop privileges
  drop_privs();

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

  // also do seccomp lockdown
  int fds[]={fileno(tempfd), fileno(batfd), fileno(cpufd), fileno(memfd), fileno(dskfd), fileno(netfd), fileno(entfd)};
  lock_seccomp(fds, 7, fileno(out));

  // run mainloop
  while(1) {
    if(out!=stdout) {
      ftruncate(fileno(out), 0);
      fseek(out,0,0);
    }
    ent();
    cpu();
    mem();
    dsk(argv[1]);
    net(argv[2]);
    temp();
    bat();
    fprintf(out, "%s", terminator);
    fflush(out);
    sleep(1);
  }
  return 0;
}
