/* Changes only this open decoder context; does not queue buffers or stream. */
#include <time.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

static void fourcc(unsigned v, char s[5]) {
    for (int i=0;i<4;i++) s[i]=(char)(v>>(8*i));
    s[4]=0;
}
static void formats(int fd,const char *stage) {
    printf("stage=%s\n",stage);
    for (unsigned i=0;;i++) {
        struct v4l2_fmtdesc d={.index=i,.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
        if (ioctl(fd,VIDIOC_ENUM_FMT,&d)<0) {
            printf("ENUM_FMT end errno=%d (%s)\n",errno,strerror(errno));break;
        }
        char f[5];fourcc(d.pixelformat,f);
        printf("CAPTURE index=%u fourcc=%s description=%s\n",i,f,d.description);
    }
    struct v4l2_format f={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    if (ioctl(fd,VIDIOC_G_FMT,&f)==0) {
        char s[5];fourcc(f.fmt.pix_mp.pixelformat,s);
        printf("G_FMT %s %ux%u planes=%u\n",s,f.fmt.pix_mp.width,f.fmt.pix_mp.height,f.fmt.pix_mp.num_planes);
    }
}
int main(void) {
    int fd=open("/dev/video22",O_RDWR|O_NONBLOCK);
    if(fd<0){perror("open");return 1;}
    struct v4l2_capability cap={0};
    if(ioctl(fd,VIDIOC_QUERYCAP,&cap)<0){perror("QUERYCAP");close(fd);return 1;}
    printf("driver=%s card=%s\n",cap.driver,cap.card);
    if(strcmp((char*)cap.driver,"iris_driver")){close(fd);return 2;}
    formats(fd,"initial");
    struct v4l2_format out={.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    out.fmt.pix_mp.width=1920;out.fmt.pix_mp.height=1080;
    out.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_HEVC;out.fmt.pix_mp.field=V4L2_FIELD_NONE;
    if(ioctl(fd,VIDIOC_S_FMT,&out)<0){perror("S_FMT HEVC");close(fd);return 1;}
    formats(fd,"after S_FMT HEVC, before headers");
    struct v4l2_queryctrl q={.id=V4L2_CID_MPEG_VIDEO_HEVC_PROFILE};
    if(ioctl(fd,VIDIOC_QUERYCTRL,&q)==0) {
        printf("HEVC profile min=%d max=%d default=%d\n",q.minimum,q.maximum,q.default_value);
        for(int i=q.minimum;i<=q.maximum;i++) {
            struct v4l2_querymenu m={.id=q.id,.index=(unsigned)i};
            if(ioctl(fd,VIDIOC_QUERYMENU,&m)==0)printf("profile=%d name=%s\n",i,m.name);
        }
    } else perror("HEVC profile query");
    unsigned wanted[]={V4L2_PIX_FMT_P010,v4l2_fourcc('Q','1','0','C')};
    for(unsigned i=0;i<2;i++) {
        struct v4l2_format f={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
        f.fmt.pix_mp.width=1920;f.fmt.pix_mp.height=1080;
        f.fmt.pix_mp.pixelformat=wanted[i];f.fmt.pix_mp.field=V4L2_FIELD_NONE;
        char req[5],got[5];fourcc(wanted[i],req);
        if(ioctl(fd,VIDIOC_TRY_FMT,&f)<0)perror("TRY_FMT");
        else{fourcc(f.fmt.pix_mp.pixelformat,got);printf("TRY_FMT requested=%s returned=%s stride=%u size=%u\n",req,got,f.fmt.pix_mp.plane_fmt[0].bytesperline,f.fmt.pix_mp.plane_fmt[0].sizeimage);}
    }
    close(fd);return 0;
}
