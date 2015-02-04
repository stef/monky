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
#include <fcntl.h>
#include <sys/stat.h>
#include "fb.h"
#include <linux/vt.h>
#include <math.h>

char font[]="./envy.ttf";

// set uid/guid to monky/readproc
#define USERID 113
#define GROUPID 30
// set uid/guid to nobody
//#define USERID 65534
//#define GROUPID 65534

// amount of history to keep
#define SAMPLES 20
#define SAMPLE_RATE 10

#define FGCOLOR 0x000000ff
#define BGCOLOR 0x000a0a0a

// this is the position of monky
#define monky_y (768 - (6*12))

#if __x86_64__
#define UD "ld"
#else
#define UD "lld"
#endif

int ttyfd;
long unsigned int iteration=0;

color_t default_colors[]={ { .limit =  0, .color = 0x000000a0, }, { .limit = NAN, .color = 0, }};

color_t percent_colors[]={ { .limit =  0, .color  = 0x00404040, },
                           { .limit = 20, .color = 0x00000080, },
                           { .limit = 50, .color = 0x00808000, },
                           { .limit = 75, .color = 0x00cccccc, },
                           { .limit = 90, .color = 0x00800000, },
                           { .limit = NAN, .color = 0, }};
color_t revpercent_colors[]={ { .limit =  0, .color = 0x00800000, },
                              { .limit = 20, .color = 0x00804000, },
                              { .limit = 50, .color = 0x00808000, },
                              { .limit = 75, .color = 0x00000080, },
                              { .limit = 90, .color = 0x00808080, },
                              { .limit = NAN, .color = 0, }};
color_t temp_colors[]={ { .limit =  0, .color = 0x00808080, },
                       { .limit = 40, .color = 0x00000080, },
                       { .limit = 60, .color = 0x00808000, },
                       { .limit = 70, .color = 0x00804000, },
                       { .limit = 80, .color = 0x00800000, },
                       { .limit = 85, .color = 0x00cccccc, },
                       { .limit = NAN, .color = 0, }};

void fail(const int cmp, const char *msg) {
   if(cmp) {
      printf("%s\n", msg);
      exit(1);
   }
}

static void push(float q[], uint8_t* idx, float *acc, const float val) {
   *acc+=val;
   if(iteration%SAMPLE_RATE==0) {
      *idx = (*idx+1) % SAMPLES;
      q[*idx]=(*acc)/SAMPLE_RATE;
      *acc=0;
   }
}

FILE* cpufd;
float cpu_samples[SAMPLES], cpuacc;
uint8_t cpu_samples_idx=0;
void cpu(void) {
  static uint64_t prevcputotal, prevcpuunused;
  uint32_t user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
  uint64_t total, unused;
  float dtotal, dunused, ret;
  char label[5];
  fail(fseek(cpufd,0,SEEK_SET)==-1, "failed to seek cpufd\n");
  fail(fscanf(cpufd,"%4s %d %d %d %d %d %d %d %d %d %d",
              label, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice)==12,
       "cannot parse cpufd\n");
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
  push(cpu_samples, &cpu_samples_idx, &cpuacc, ret);
  char buf[32];
  size_t buflen;
  buflen=snprintf(buf,32,"c:%4.1f%%.", ret);
  display(buf, buflen, cpu_samples, cpu_samples_idx, SAMPLES, FGCOLOR, BGCOLOR, percent_colors);
}

FILE* entfd;
float ent_samples[SAMPLES], entacc;
uint8_t ent_samples_idx=0;
void ent(void) {
  uint32_t avail=0;
  fail(fseek(entfd,0,SEEK_SET)==-1, "failed to seek entfd\n");
  fail(fscanf(entfd,"%d\n", &avail)!=1,"failed to parse ent\n");
  push(ent_samples, &ent_samples_idx, &entacc, (float) avail);
  char buf[32];
  size_t buflen;
  buflen=snprintf(buf,32,".e: %4d.", avail);
  display(buf, buflen, ent_samples, ent_samples_idx, SAMPLES, FGCOLOR, BGCOLOR, default_colors);
}

FILE *tempfd;
float temp_samples[SAMPLES], tempacc;
uint8_t temp_samples_idx=0;
void temp(void) {
  uint32_t t=0;
  fail(fseek(tempfd,0,SEEK_SET)==-1, "failed to seek tempfd\n");
  fail(fscanf(tempfd,"%d\n", &t)!=1, "failed to parse tempfd\n");
  push(temp_samples, &temp_samples_idx, &tempacc, ((float)t)/1000.0);
  char buf[32];
  size_t buflen;
  buflen=snprintf(buf,32,".t:%2dC.", t/1000);
  display(buf, buflen, temp_samples, temp_samples_idx, SAMPLES, FGCOLOR, BGCOLOR, temp_colors);
}

FILE* memfd;
float mem_samples[SAMPLES], memacc;
uint8_t mem_samples_idx=0;
void mem(void) {
  float max=0, cur=0, ret;
  uint64_t val;
  char label[15];
  fail(fseek(memfd,0,SEEK_SET)==-1, "failed to seek memfd\n");
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
  push(mem_samples, &mem_samples_idx, &memacc, ret);
  char buf[32];
  size_t buflen;
  buflen=snprintf(buf,32,".m:%5.1f%%.", ret);
  display(buf, buflen, mem_samples, mem_samples_idx, SAMPLES, FGCOLOR, BGCOLOR, percent_colors);
}

FILE* batfd;
float bat_samples[SAMPLES], batacc;
uint8_t bat_samples_idx=0;
void bat() {
  float max=0, cur=0, ret;
  char val[33], status;
  char label[129];
  status=0;
  fail(fseek(batfd,0,SEEK_SET)==-1, "failed to seek batfd\n");
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
  push(bat_samples, &bat_samples_idx, &batacc, ret);
  char buf[32];
  size_t buflen;
  buflen=snprintf(buf,32,".b: %5.1f%%:%c.", ret, status);
  display(buf, buflen, bat_samples, bat_samples_idx, SAMPLES, FGCOLOR, BGCOLOR, revpercent_colors);
}

FILE* dskfd;
float dsk_samples[SAMPLES], dskacc;
uint8_t dsk_samples_idx=0;
void dsk(const char* dev) {
  if(dev==NULL) return;
  static uint64_t previo;
  uint64_t reads, writes, total, ret;
  char label[11];
  fail(fseek(dskfd,0,SEEK_SET)==-1, "failed to seek dskfd\n");
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
  push(dsk_samples, &dsk_samples_idx, &dskacc, ret);
  char buf[33];
  size_t buflen;
  buflen=snprintf(buf,32,".io: %5"UD"kB/s.", ret);
  display(buf, buflen, dsk_samples, dsk_samples_idx, SAMPLES, FGCOLOR, BGCOLOR, default_colors);
}

FILE* netfd;
uint64_t pup, pdown;
float up_samples[SAMPLES], down_samples[SAMPLES], upacc, downacc;
void net(const char* dev) {
  if(dev==NULL) return;
  static uint8_t up_samples_idx=0, down_samples_idx=0;
  float up, down;
  uint64_t read=0, write=0;
  char label[11]="";

  fail(fseek(netfd,0,SEEK_SET)==-1, "failed to seek netfd\n");
  while(strncmp(label, dev, sizeof(label))!=0) {
    if(fscanf(netfd," %10s %"UD" %*d %*d %*d %*d %*d %*d %*d %"UD" %*d %*d %*d %*d %*d %*d %*d\n", label, &read, &write)==EOF) {
      break;
    }
  }
  while(fgetc(netfd)!=EOF); // weird way of flushing /proc files
  if(strncmp(dev,label,sizeof(label))!=0) {
    // device does not exist, push 0 into history and return
    push(up_samples, &up_samples_idx, &upacc, 0);
    push(down_samples, &down_samples_idx, &downacc, 0);
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

  push(up_samples, &up_samples_idx, &upacc, up);
  push(down_samples, &down_samples_idx, &downacc, down);

  // remember current sample
  pup=write;
  pdown=read;
  char buf[33];
  size_t buflen;
  buflen=snprintf(buf,32,".u:%6.1fkB/s.", up);
  display(buf, buflen, up_samples, up_samples_idx, SAMPLES, FGCOLOR, BGCOLOR, default_colors);
  buflen=snprintf(buf,32,".d:%6.1fkB/s.", down);
  display(buf, buflen, down_samples, down_samples_idx, SAMPLES, FGCOLOR, BGCOLOR, default_colors);
}

#ifndef WITHOUT_SECCOMP
void lock_seccomp(const int fds[], const size_t fdsize, const int fbfd) {
  // Init the filter
  int i;
  scmp_filter_ctx ctx;
  ctx = seccomp_init(SCMP_ACT_KILL); // default action: kill

#if !defined __GLIBC__ // assume musl
  //readv([3,4,5,6,7,8,9]
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readv), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));
  //lseek([3,4,5,6,7,8,9], 0, [0], SEEK_SET
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));

  //mmap(NULL, 3145728, PROT_READ|PROT_WRITE, MAP_SHARED, 10, 0)
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 1,
                        SCMP_A4(SCMP_CMP_EQ, fbfd));
  //mmap(NULL, 89924, PROT_READ, MAP_PRIVATE, 11, 0)
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 1,
                        SCMP_A4(SCMP_CMP_EQ, fbfd+1));
  //open("/home/s/mon/dev/envy.ttf", O_RDONLY) = 11

  //fcntl(11, F_SETFD, FD_CLOEXEC)    = 0
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl), 1,
                        SCMP_A0(SCMP_CMP_EQ, fbfd+1));
  //fstat(11, {st_mode=S_IFREG|0644, st_size=89924, ...}) = 0
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fstat), 1,
                        SCMP_A0(SCMP_CMP_EQ, fbfd+1));
  //close(11
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(close), 1,
                        SCMP_A0(SCMP_CMP_EQ, fbfd+1));
  //writev(1
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(writev), 1,
                        SCMP_A0(SCMP_CMP_EQ, 1));
#else //__GLIBC__
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

  //read([3,4,5,6,7,8,9]
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(read), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));

  //_llseek([3,4,5,6,7,8,9], 0, [0], SEEK_SET
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(_llseek), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));

  //fstat64([3,4,5,6,7,8,9], {st_mode=S_IFREG|0444, st_size=4096, ...}) = 0
  for(i=0;i<fdsize;i++) seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fstat64), 1,
                                              SCMP_A0(SCMP_CMP_EQ, fds[i]));

#endif

  //ioctl(ttyfd, VT_GETSTATE, &vtstat
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 2,
                        SCMP_A0(SCMP_CMP_EQ, ttyfd),
                        SCMP_A1(SCMP_CMP_EQ, VT_GETSTATE));

  //ioctl(fbfd
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
                        SCMP_A0(SCMP_CMP_EQ, fbfd));

  //nanosleep({1, 0}
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(nanosleep), 0);

  // brk
  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(brk), 0);

  seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(exit_group), 0);

  // enable seccomp rules
  seccomp_load(ctx);
}
#endif // without_seccomp

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
    printf("ERROR: Managed to regain root privileges.");
    exit(1);
  }
}

int main(const int argc, const char** argv) {
  if(argc<3) {
    printf("usage: %s diskdev netdev:\n", argv[0]);
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
  int fbfd;
  fbfd=open("/dev/fb0", O_RDWR);
  ttyfd=open("/dev/tty0",O_RDONLY);

  // drop privileges
  drop_privs();

  // read in font file into memory, leave the parsing after the sandboxing
  struct stat st;
  if(stat(font, &st)==-1) {
    printf("couldn't stat %s\n", font);
    exit(1);
  }
  if(st.st_size>1024*1024*10) { // 10M fontfile is a generous limit
    printf("%s too big.\n", font);
    exit(1);
  }
  char *ttfp;
  if((ttfp=malloc(st.st_size))==NULL) {
    printf("couldn't malloc %ld byte for font.\n", st.st_size);
    exit(1);
  }
  int ttffd;
  if((ttffd=open(font,O_RDONLY))==-1) {
    printf("couldn't open %s.\n", font);
    exit(1);
  }
  int ret;
  if((ret=read(ttffd, ttfp, st.st_size))!=st.st_size) {
    printf("didn't read complete font, only %d bytes read out of %ld.\n", ret, st.st_size);
    exit(1);
  }
  close(ttffd);

#ifndef WITHOUT_SECCOMP
  // do seccomp sandbox
  int fds[]={fileno(tempfd), fileno(batfd), fileno(cpufd), fileno(memfd), fileno(dskfd), fileno(netfd), fileno(entfd)};
  lock_seccomp(fds, 8, fbfd);
#endif

  // initialize framebuffer and font rendering
  init_fb(fbfd, ttyfd);
  init_ft(ttfp, st.st_size);

  // run mainloop
  while(1) {
    // right aligned
    newline(-1, monky_y, BGCOLOR);
    ent();
    net(argv[2]);
    dsk(argv[1]);
    bat();
    temp();
    mem();
    cpu();
    sleep(1);
    iteration++;
  }
  return 0;
}
