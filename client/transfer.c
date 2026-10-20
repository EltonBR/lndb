#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "lndb_protocol.h"
#include "transfer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *base_name(const char *p)
{
    char *copy = strdup(p);
    if (!copy) return NULL;
    size_t n = strlen(copy);
    while (n > 1 && copy[n - 1] == '/') copy[--n] = '\0';
    char *s = strrchr(copy, '/');
    char *name = strdup(s ? s + 1 : copy);
    free(copy);
    if (name && !*name) { free(name); return strdup("."); }
    return name;
}
static int safe_rel(const char *p)
{
    if (!p || !*p || *p == '/') return 0;
    const char *s = p;
    while (*s) { const char *e = strchr(s, '/'); size_t n = e ? (size_t)(e-s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        if (!e) break;
        s = e + 1;
    }
    return 1;
}
static int send_meta(int fd, unsigned kind, uint32_t mode, uint64_t size, const char *rel)
{
    size_t n = strlen(rel); if (n > 65535 || n + 15 > LNDB_FRAME_MAX_PAYLOAD) return -1;
    uint8_t *p = malloc(n + 15); if (!p) return -1;
    p[0] = (uint8_t)kind; lndb_put_u32(p + 1, mode);
    for (unsigned i=0;i<8;i++) p[5+i]=(uint8_t)(size >> (56-8*i));
    p[13]=(uint8_t)(n>>8); p[14]=(uint8_t)n; memcpy(p+15,rel,n);
    int r=lndb_send_frame(fd,LNDB_MSG_FILE_META,p,(uint32_t)(n+15)); free(p); return r;
}
static int send_file(int fd, const char *local, const char *rel, const struct stat *st)
{
    if (send_meta(fd,1,(uint32_t)(st->st_mode & 0777),(uint64_t)st->st_size,rel)<0) return -1;
    int in=open(local,O_RDONLY|O_NOFOLLOW); if(in<0)return -1;
    uint8_t b[65536]; int rc=0;
    for (;;) { ssize_t n=read(in,b,sizeof(b)); if(n<0&&errno==EINTR)continue; if(n<0){rc=-1;break;} if(!n)break;
        if(lndb_send_frame(fd,LNDB_MSG_FILE_DATA,b,(uint32_t)n)<0){rc=-1;break;} }
    close(in); if(rc<0)return rc; return lndb_send_frame(fd,LNDB_MSG_FILE_END,NULL,0);
}
static int send_tree(int fd,const char *local,const char *rel)
{
    struct stat st; if(lstat(local,&st)<0)return -1;
    if(S_ISLNK(st.st_mode)||(!S_ISDIR(st.st_mode)&&!S_ISREG(st.st_mode))){errno=EINVAL;return -1;}
    if(S_ISREG(st.st_mode))return send_file(fd,local,rel,&st);
    if(send_meta(fd,2,(uint32_t)(st.st_mode&0777),0,rel)<0)return -1;
    DIR *d=opendir(local);if(!d)return -1; struct dirent *de; int rc=0;
    while((de=readdir(d))){if(!strcmp(de->d_name,".")||!strcmp(de->d_name,".."))continue;
        size_t a=strlen(rel),b=strlen(de->d_name); char *child=malloc(a+b+2),*path=malloc(strlen(local)+b+2);
        if(!child||!path){free(child);free(path);rc=-1;break;}
        sprintf(child,"%s/%s",rel,de->d_name);sprintf(path,"%s/%s",local,de->d_name);
        rc=send_tree(fd,path,child);free(child);free(path);if(rc<0)break;
    }
    closedir(d);return rc;
}
static int mkdir_parents(const char *path)
{
    char *p=strdup(path);if(!p)return -1;
    for(char *s=p+1;*s;s++)if(*s=='/'){*s=0;if(mkdir(p,0755)<0&&errno!=EEXIST){free(p);return -1;}*s='/';}
    free(p);return 0;
}
static char *join_path(const char *a,const char *b)
{ size_t n=strlen(a)+strlen(b)+2;char *p=malloc(n);if(p)snprintf(p,n,"%s/%s",a,b);return p; }
static int receive_tree(int fd,const char *dest,const char *root,int strip_root)
{
    for(;;){uint8_t type,*p=NULL;uint32_t n=0;if(lndb_recv_frame(fd,&type,&p,&n)<0)return -1;
        if(type==LNDB_MSG_FILE_DONE){int status=(n==4)?(int)lndb_get_u32(p):1;free(p);return status? -1:0;}
        if(type==LNDB_MSG_ERROR){if(n)(void)lndb_write_full(STDERR_FILENO,p,n);free(p);return -1;}
        if(type!=LNDB_MSG_FILE_META||n<15){free(p);return -1;}
        unsigned kind=p[0];uint32_t mode=lndb_get_u32(p+1);uint64_t size=0;for(unsigned i=0;i<8;i++)size=(size<<8)|p[5+i];
        size_t plen=((size_t)p[13]<<8)|p[14]; if(plen+15!=n){free(p);return -1;}
        char *rel=malloc(plen+1);if(!rel){free(p);return -1;}memcpy(rel,p+15,plen);rel[plen]=0;
        if (memchr(rel, 0, plen)) { free(rel); free(p); return -1; }
        free(p);
        if(kind==0){free(rel);continue;}
        if(!safe_rel(rel)){free(rel);return -1;}
        const char *mapped=rel;
        if(strip_root){size_t rlen=strlen(root);if(strcmp(rel,root)==0)mapped="";
            else if(strncmp(rel,root,rlen)==0&&rel[rlen]=='/')mapped=rel+rlen+1;else{free(rel);return -1;}}
        char *path;
        if(!*mapped)path=strdup(dest);else path=join_path(dest,mapped);
        free(rel);if(!path)return -1;
        if(kind==2){if(mkdir_parents(path)<0||(mkdir(path,mode&0777)<0&&errno!=EEXIST)||chmod(path,mode&0777)<0){free(path);return -1;}free(path);continue;}
        if(kind!=1||size>INT64_MAX||mkdir_parents(path)<0){free(path);return -1;}
        size_t tn=strlen(path)+16;char *tmp=malloc(tn);if(!tmp){free(path);return -1;}snprintf(tmp,tn,"%s.lndbXXXXXX",path);
        int out=mkstemp(tmp);if(out<0){free(tmp);free(path);return -1;}
        uint64_t got=0;int bad=0;
        while(got<size){uint8_t t,*data=NULL;uint32_t z=0;if(lndb_recv_frame(fd,&t,&data,&z)<0){bad=1;break;}
            if(t!=LNDB_MSG_FILE_DATA||!z||z>size-got||lndb_write_full(out,data,z)<0)bad=1;else got+=z;free(data);if(bad)break;}
        uint8_t t,*end=NULL;uint32_t z=0;if(!bad&&(lndb_recv_frame(fd,&t,&end,&z)<0||t!=LNDB_MSG_FILE_END||z))bad=1;free(end);
        if(fchmod(out,mode&0777)<0) bad=1;
        if(close(out)<0) bad=1;
        if(!bad&&rename(tmp,path)<0) bad=1;
        if(bad) unlink(tmp);
        free(tmp); free(path);
        if(bad) return -1;
    }
}
int lndb_client_transfer(int fd,int push,const char *src,const char *dst)
{
    const char *remote=push?dst:src;size_t rn=strlen(remote);if(!rn||rn+1>LNDB_FRAME_MAX_PAYLOAD)return -1;
    uint8_t *req=malloc(rn+1);if(!req)return -1;req[0]=push?1:2;memcpy(req+1,remote,rn);
    int rc=lndb_send_frame(fd,LNDB_MSG_FILE_REQUEST,req,(uint32_t)(rn+1));free(req);if(rc<0)return -1;
    if(push){char *source=strdup(src);if(!source)return -1;size_t sn=strlen(source);while(sn>1&&source[sn-1]=='/')source[--sn]='\0';
        struct stat st;if(lstat(source,&st)<0){free(source);return -1;}char *rel=base_name(source);if(!rel){free(source);return -1;}if(!safe_rel(rel)){free(rel);free(source);return -1;}
        int sent=send_tree(fd,source,rel);free(rel);free(source);if(sent<0)return -1;
        if(send_meta(fd,0,0,0,"")<0) return -1;
        uint8_t t,*p=NULL;uint32_t n=0;if(lndb_recv_frame(fd,&t,&p,&n)<0)return -1;
        rc=(t==LNDB_MSG_FILE_DONE&&n==4)?(int)lndb_get_u32(p):-1;free(p);return rc;
    }
    struct stat st;int exists=lstat(dst,&st)==0;int is_dir=exists&&S_ISDIR(st.st_mode);
    char *dest=strdup(dst);if(!dest)return -1;
    if(mkdir_parents(dest)<0){free(dest);return -1;}
    rc=receive_tree(fd,dest,base_name(src),!is_dir);free(dest);return rc;
}
