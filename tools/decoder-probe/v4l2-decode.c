/* Bounded stateful HEVC probe for the approved Iris device. No software decoder. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUFS 32
#define MAX_PACKETS 256
struct mapped { void *ptr[VIDEO_MAX_PLANES]; size_t len[VIDEO_MAX_PLANES]; unsigned planes; };
struct packet { size_t offset,size; };
static int fd=-1;
static void die(const char *s) {perror(s);exit(2);}
static int call(unsigned long op,void *p) {int r;do{r=ioctl(fd,op,p);}while(r<0&&errno==EINTR);return r;}
static void must(unsigned long op,void *p,const char *s){if(call(op,p)<0)die(s);}
static void fourcc(uint32_t x,char s[5]) {for(int i=0;i<4;i++)s[i]=(char)(x>>(8*i));s[4]=0;}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void describe(struct v4l2_format *f,const char *stage) {
    char s[5];fourcc(f->fmt.pix_mp.pixelformat,s);
    printf("%s format=%s width=%u height=%u planes=%u colorspace=%u xfer=%u ycbcr=%u quant=%u\n",
        stage,s,f->fmt.pix_mp.width,f->fmt.pix_mp.height,f->fmt.pix_mp.num_planes,
        f->fmt.pix_mp.colorspace,f->fmt.pix_mp.xfer_func,f->fmt.pix_mp.ycbcr_enc,f->fmt.pix_mp.quantization);
    for(unsigned i=0;i<f->fmt.pix_mp.num_planes;i++)
        printf("plane=%u stride=%u sizeimage=%u\n",i,f->fmt.pix_mp.plane_fmt[i].bytesperline,f->fmt.pix_mp.plane_fmt[i].sizeimage);
}
static void enumerate(const char *stage) {
    printf("enumeration=%s\n",stage);
    for(unsigned i=0;;i++){
        struct v4l2_fmtdesc d={.index=i,.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
        if(call(VIDIOC_ENUM_FMT,&d)<0){printf("enum_end errno=%d\n",errno);break;}
        char s[5];fourcc(d.pixelformat,s);printf("enum_capture=%s description=%s\n",s,d.description);
    }
}
static unsigned allocate(enum v4l2_buf_type type,unsigned count,struct mapped *m) {
    struct v4l2_requestbuffers r={.count=count,.type=type,.memory=V4L2_MEMORY_MMAP};
    must(VIDIOC_REQBUFS,&r,"REQBUFS");
    printf("REQBUFS type=%u count=%u capabilities=0x%x\n",type,r.count,r.capabilities);
    if(!r.count||r.count>MAX_BUFS){fprintf(stderr,"unexpected buffer count\n");exit(2);}
    for(unsigned i=0;i<r.count;i++){
        struct v4l2_plane p[VIDEO_MAX_PLANES]={0};
        struct v4l2_buffer b={.type=type,.memory=V4L2_MEMORY_MMAP,.index=i,.length=VIDEO_MAX_PLANES,.m.planes=p};
        must(VIDIOC_QUERYBUF,&b,"QUERYBUF");m[i].planes=b.length;
        if(!b.length||b.length>VIDEO_MAX_PLANES){fprintf(stderr,"invalid planes\n");exit(2);}
        for(unsigned j=0;j<b.length;j++){
            m[i].len[j]=p[j].length;
            m[i].ptr[j]=mmap(NULL,p[j].length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,p[j].m.mem_offset);
            if(m[i].ptr[j]==MAP_FAILED)die("mmap");
            printf("QUERYBUF type=%u index=%u plane=%u length=%u mmap_offset=%u\n",type,i,j,p[j].length,p[j].m.mem_offset);
        }
    }
    return r.count;
}
static void queue(unsigned type,unsigned i,struct mapped *m,const uint8_t *data,size_t size,unsigned seq) {
    struct v4l2_plane p[VIDEO_MAX_PLANES]={0};
    struct v4l2_buffer b={.type=type,.memory=V4L2_MEMORY_MMAP,.index=i,.length=m[i].planes,.m.planes=p};
    for(unsigned j=0;j<b.length;j++)p[j].length=(unsigned)m[i].len[j];
    if(data){
        if(size>m[i].len[0]){fprintf(stderr,"packet exceeds OUTPUT allocation\n");exit(2);}
        memcpy(m[i].ptr[0],data,size);p[0].bytesused=(unsigned)size;
        b.timestamp.tv_sec=seq/30;b.timestamp.tv_usec=(seq%30)*1000000/30;
    }
    must(VIDIOC_QBUF,&b,"QBUF");
}
static unsigned packets(const uint8_t *data,size_t size,struct packet *p) {
    size_t starts[MAX_PACKETS];unsigned n=0;
    for(size_t i=0;i+5<size;){
        unsigned prefix=0;
        if(data[i]==0&&data[i+1]==0&&data[i+2]==1)prefix=3;
        else if(data[i]==0&&data[i+1]==0&&data[i+2]==0&&data[i+3]==1)prefix=4;
        if(prefix){
            if(((data[i+prefix]>>1)&63)==35){
                if(n==MAX_PACKETS){fprintf(stderr,"too many AUs\n");exit(2);}
                starts[n++]=i;
            }
            i+=prefix+2;
        }else i++;
    }
    if(!n){fprintf(stderr,"fixture requires AUD-delimited AUs\n");exit(2);}
    starts[0]=0;
    for(unsigned i=0;i<n;i++){p[i].offset=starts[i];p[i].size=(i+1<n?starts[i+1]:size)-starts[i];}
    return n;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc!=4){fprintf(stderr,"usage: v4l2-decode INPUT {auto|NV12|Q10C|P010|after-FOURCC} DUMP_FIRST_ALLOCATION\n");return 2;}
    uint32_t preferred=0,after=0;
    /* after-*: keep the driver's choice until the headers are parsed, then
     * request the format (the driver knows the bit depth only from then). */
    if(!strncmp(argv[2],"after-",6)&&strlen(argv[2])==10)
        after=v4l2_fourcc(argv[2][6],argv[2][7],argv[2][8],argv[2][9]);
    else if(!strcmp(argv[2],"NV12"))preferred=V4L2_PIX_FMT_NV12;
    else if(!strcmp(argv[2],"Q10C"))preferred=v4l2_fourcc('Q','1','0','C');
    else if(!strcmp(argv[2],"P010"))preferred=V4L2_PIX_FMT_P010;
    else if(strcmp(argv[2],"auto"))return 2;
    FILE *in=fopen(argv[1],"rb");if(!in)die("input");
    if(fseek(in,0,SEEK_END))die("seek");
    long flen=ftell(in);
    if(flen<=0||flen>16*1024*1024){fprintf(stderr,"input size outside bound\n");return 2;}
    rewind(in);uint8_t *data=malloc((size_t)flen);if(!data)die("malloc");
    if(fread(data,1,(size_t)flen,in)!=(size_t)flen)die("read");
    fclose(in);
    struct packet pk[MAX_PACKETS];unsigned total=packets(data,(size_t)flen,pk);
    printf("input_AUs=%u bytes=%ld requested=%s\n",total,flen,argv[2]);
    if(total!=60){fprintf(stderr,"expected exactly 60 fixture AUs\n");return 2;}
    fd=open("/dev/video-dec0",O_RDWR|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)fd=open("/dev/video22",O_RDWR|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)die("open decoder");
    struct v4l2_capability cap={0};must(VIDIOC_QUERYCAP,&cap,"QUERYCAP");
    printf("driver=%s card=%s\n",cap.driver,cap.card);
    if(strcmp((char*)cap.driver,"iris_driver")){fprintf(stderr,"unexpected driver\n");return 2;}
    struct v4l2_event_subscription sub={.type=V4L2_EVENT_SOURCE_CHANGE};must(VIDIOC_SUBSCRIBE_EVENT,&sub,"SUBSCRIBE SOURCE_CHANGE");
    sub.type=V4L2_EVENT_EOS;must(VIDIOC_SUBSCRIBE_EVENT,&sub,"SUBSCRIBE EOS");
    struct v4l2_format f={.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    f.fmt.pix_mp.width=1920;f.fmt.pix_mp.height=1080;f.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_HEVC;
    f.fmt.pix_mp.field=V4L2_FIELD_NONE;f.fmt.pix_mp.num_planes=1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage=1024*1024;
    must(VIDIOC_S_FMT,&f,"S_FMT OUTPUT");describe(&f,"OUTPUT");enumerate("after HEVC S_FMT");
    struct v4l2_format capture={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    if(preferred){
        capture.fmt.pix_mp.width=1920;capture.fmt.pix_mp.height=1080;
        capture.fmt.pix_mp.pixelformat=preferred;capture.fmt.pix_mp.field=V4L2_FIELD_NONE;
        must(VIDIOC_S_FMT,&capture,"S_FMT initial CAPTURE");describe(&capture,"initial CAPTURE");
        if(capture.fmt.pix_mp.pixelformat!=preferred){fprintf(stderr,"requested format substituted; no streaming\n");return 3;}
    }
    struct mapped output[MAX_BUFS]={0},cm[MAX_BUFS]={0};
    unsigned no=allocate(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,4,output),nc=0,next=0,done=0,frames=0,errors=0,exports=0;
    for(unsigned i=0;i<no&&next<total;i++,next++)queue(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,i,output,data+pk[next].offset,pk[next].size,next);
    enum v4l2_buf_type type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;must(VIDIOC_STREAMON,&type,"STREAMON OUTPUT");
    int ready=0,stop=0,eos=0,last=0;double deadline=seconds()+20;
    while(seconds()<deadline && !last){
        struct pollfd pollfd={.fd=fd,.events=POLLIN|POLLOUT|POLLPRI};
        int pr=poll(&pollfd,1,100);if(pr<0&&errno!=EINTR)die("poll");
        struct v4l2_event ev={0};
        while(call(VIDIOC_DQEVENT,&ev)==0){
            printf("event=%u changes=0x%x\n",ev.type,ev.u.src_change.changes);
            if(ev.type==V4L2_EVENT_EOS)eos=1;
            if(ev.type==V4L2_EVENT_SOURCE_CHANGE){
                if(ready){fprintf(stderr,"unexpected additional source change\n");errors++;goto finish;}
                capture.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;must(VIDIOC_G_FMT,&capture,"G_FMT after headers");
                describe(&capture,"after SOURCE_CHANGE");enumerate("after SOURCE_CHANGE");
                if(after){
                    struct v4l2_format tried=capture;tried.fmt.pix_mp.pixelformat=after;
                    if(call(VIDIOC_TRY_FMT,&tried)==0)describe(&tried,"TRY_FMT after headers");
                    else printf("TRY_FMT after headers failed errno=%d (%s)\n",errno,strerror(errno));
                    capture.fmt.pix_mp.pixelformat=after;
                    if(call(VIDIOC_S_FMT,&capture)<0){printf("S_FMT after headers failed errno=%d (%s)\n",errno,strerror(errno));errors++;goto finish;}
                    describe(&capture,"S_FMT after headers");
                    if(capture.fmt.pix_mp.pixelformat!=after){fprintf(stderr,"requested format substituted after headers\n");errors++;goto finish;}
                }
                struct v4l2_selection sel={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,.target=V4L2_SEL_TGT_COMPOSE};
                if(call(VIDIOC_G_SELECTION,&sel)==0)printf("compose left=%d top=%d width=%u height=%u\n",sel.r.left,sel.r.top,sel.r.width,sel.r.height);
                struct v4l2_control min={.id=V4L2_CID_MIN_BUFFERS_FOR_CAPTURE};
                unsigned count=12;
                if(call(VIDIOC_G_CTRL,&min)==0&&min.value>0){count=(unsigned)min.value+2;printf("min_capture=%d\n",min.value);}
                if(count>MAX_BUFS){fprintf(stderr,"capture count outside bound\n");errors++;goto finish;}
                nc=allocate(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,count,cm);
                for(unsigned i=0;i<nc;i++){
                    for(unsigned j=0;j<cm[i].planes;j++){
                        struct v4l2_exportbuffer ex={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,.index=i,.plane=j,.flags=O_CLOEXEC|O_RDONLY};
                        if(call(VIDIOC_EXPBUF,&ex)==0){
                            struct stat st={0};int sr=fstat(ex.fd,&st);exports++;
                            printf("EXPBUF index=%u plane=%u success fd=%d fstat=%d size=%lld\n",i,j,ex.fd,sr,(long long)st.st_size);close(ex.fd);
                        }else printf("EXPBUF index=%u plane=%u failed errno=%d (%s)\n",i,j,errno,strerror(errno));
                    }
                    queue(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,i,cm,NULL,0,0);
                }
                type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;must(VIDIOC_STREAMON,&type,"STREAMON CAPTURE");ready=1;
            }
        }
        for(;;){
            struct v4l2_plane p[VIDEO_MAX_PLANES]={0};
            struct v4l2_buffer b={.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,.memory=V4L2_MEMORY_MMAP,.length=VIDEO_MAX_PLANES,.m.planes=p};
            if(call(VIDIOC_DQBUF,&b)<0){if(errno!=EAGAIN)perror("DQBUF OUTPUT");break;}
            done++;
            if(b.flags&V4L2_BUF_FLAG_ERROR){errors++;printf("OUTPUT error index=%u\n",b.index);}
            if(next<total){queue(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,b.index,output,data+pk[next].offset,pk[next].size,next);next++;}
        }
        if(next==total&&!stop){struct v4l2_decoder_cmd cmd={.cmd=V4L2_DEC_CMD_STOP};must(VIDIOC_DECODER_CMD,&cmd,"DECODER STOP/drain");stop=1;}
        if(ready)for(;;){
            struct v4l2_plane p[VIDEO_MAX_PLANES]={0};
            struct v4l2_buffer b={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,.memory=V4L2_MEMORY_MMAP,.length=VIDEO_MAX_PLANES,.m.planes=p};
            if(call(VIDIOC_DQBUF,&b)<0){if(errno!=EAGAIN)perror("DQBUF CAPTURE");break;}
            if(b.flags&V4L2_BUF_FLAG_ERROR)errors++;
            if(p[0].bytesused){
                frames++;
                printf("frame=%u index=%u sequence=%u flags=0x%x bytesused=%u data_offset=%u timestamp=%ld.%06ld\n",frames,b.index,b.sequence,b.flags,p[0].bytesused,p[0].data_offset,(long)b.timestamp.tv_sec,(long)b.timestamp.tv_usec);
                if(frames==1){
                    FILE *dump=fopen(argv[3],"wb");if(!dump)die("dump open");
                    for(unsigned j=0;j<b.length;j++){
                        if(p[j].bytesused>cm[b.index].len[j]||p[j].data_offset>p[j].bytesused){fprintf(stderr,"invalid dump bounds\n");exit(2);}
                        size_t sz=p[j].bytesused-p[j].data_offset;
                        if(fwrite((uint8_t*)cm[b.index].ptr[j]+p[j].data_offset,1,sz,dump)!=sz)die("dump write");
                    }
                    if(fclose(dump))die("dump close");
                }
            }
            if(b.flags&V4L2_BUF_FLAG_LAST){last=1;break;}
            queue(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,b.index,cm,NULL,0,0);
        }
        if(eos&&frames>=total)break;
    }
finish:
    printf("result queued=%u returned_input=%u frames=%u errors=%u exported_planes=%u eos=%d last=%d\n",next,done,frames,errors,exports,eos,last);
    if(ready){type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;call(VIDIOC_STREAMOFF,&type);}
    type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;call(VIDIOC_STREAMOFF,&type);
    for(unsigned i=0;i<no;i++)for(unsigned j=0;j<output[i].planes;j++)munmap(output[i].ptr[j],output[i].len[j]);
    for(unsigned i=0;i<nc;i++)for(unsigned j=0;j<cm[i].planes;j++)munmap(cm[i].ptr[j],cm[i].len[j]);
    close(fd);free(data);
    return frames==total&&!errors?0:1;
}
