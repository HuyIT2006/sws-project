#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define VERSION "sws/1.0"
#define BUFFER_SIZE 8192

static int debug_mode = 0;
static FILE *log_file = NULL;
static char *document_root;
static char *cgi_root = NULL;

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-dh] [-c dir] [-i address] [-l file] [-p port] dir\n", prog);
}

static void sigchld_handler(int unused) {
    (void)unused;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

static int send_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static void format_time(time_t t, char *out, size_t size) {
    struct tm tm_storage;
    gmtime_r(&t, &tm_storage);
    strftime(out, size, "%a, %d %b %Y %H:%M:%S GMT", &tm_storage);
}

static const char *content_type(const char *path) {
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    if (!strcasecmp(e, ".html") || !strcasecmp(e, ".htm")) return "text/html";
    if (!strcasecmp(e, ".css")) return "text/css";
    if (!strcasecmp(e, ".js")) return "application/javascript";
    if (!strcasecmp(e, ".txt")) return "text/plain; charset=UTF-8";
    if (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".jpe")) return "image/jpeg";
    if (!strcasecmp(e, ".png")) return "image/png";
    if (!strcasecmp(e, ".gif")) return "image/gif";
    if (!strcasecmp(e, ".svg")) return "image/svg+xml";
    if (!strcasecmp(e, ".pdf")) return "application/pdf";
    if (!strcasecmp(e, ".xml")) return "application/xml";
    if (!strcasecmp(e, ".json")) return "application/json";
    return "application/octet-stream";
}

static int make_headers(char *out, size_t size, int status, const char *reason,
                        const char *type, off_t length, struct stat *st, int include_connection) {
    char date[64], lm[64];
    format_time(time(NULL), date, sizeof(date));
    if (st) format_time(st->st_mtime, lm, sizeof(lm)); else strcpy(lm, "Thu, 01 Jan 1970 00:00:00 GMT");
    (void)include_connection;
    return snprintf(out, size,
        "HTTP/1.0 %d %s\r\nDate: %s\r\nServer: %s\r\nLast-Modified: %s\r\n"
        "Content-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
        status, reason, date, VERSION, lm, type ? type : "text/html", (long long)length);
}

static void send_error(int fd, int status, const char *reason, const char *body) {
    char headers[1024];
    char full_body[1024];
    snprintf(full_body, sizeof(full_body),
             "<!DOCTYPE html><html><body><h1>%d %s</h1><p>%s</p></body></html>",
             status, reason, body);
    size_t n = strlen(full_body);
    int hl = make_headers(headers, sizeof(headers), status, reason, "text/html; charset=UTF-8", (off_t)n, NULL, 1);
    if (hl > 0) send_all(fd, headers, (size_t)hl);
    send_all(fd, full_body, n);
}

static void log_request(const char *remote, const char *request_line, int status, long long bytes) {
    if (!log_file && !debug_mode) return;
    char t[64]; format_time(time(NULL), t, sizeof(t));
    char iso[64];
    struct tm tm_storage; time_t now = time(NULL); gmtime_r(&now, &tm_storage);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_storage);
    FILE *out = debug_mode ? stdout : log_file;
    fprintf(out, "%s [%s] \"%s\" %d %lld\n", remote, iso, request_line, status, bytes);
    fflush(out);
}

static int read_request(int fd, char *buf, size_t size) {
    size_t used = 0;
    while (used + 1 < size) {
        ssize_t n = recv(fd, buf + used, size - 1 - used, 0);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) break;
    }
    return used ? 0 : -1;
}

static int parse_modified(const char *value, time_t *result) {
    struct tm tm_storage;
    memset(&tm_storage, 0, sizeof(tm_storage));
    if (!strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &tm_storage)) return -1;
    *result = timegm(&tm_storage);
    return 0;
}

static const char *header_value(const char *request, const char *name) {
    size_t name_len = strlen(name);
    const char *line = request;
    while (*line) {
        while (*line == '\r' || *line == '\n') line++;
        const char *colon = strchr(line, ':');
        if (!colon) return NULL;
        if ((size_t)(colon - line) == name_len && strncasecmp(line, name, name_len) == 0) {
            const char *value = colon + 1;
            while (*value == ' ' || *value == '\t') value++;
            return value;
        }
        line = strchr(line, '\n');
        if (!line) return NULL;
        line++;
    }
    return NULL;
}

static void html_escape(char *dst, size_t size, const char *src) {
    size_t k = 0;
    for (; *src && k + 1 < size; src++) {
        const char *rep = NULL;
        if (*src == '&') rep = "&amp;";
        else if (*src == '<') rep = "&lt;";
        else if (*src == '>') rep = "&gt;";
        if (rep) { while (*rep && k + 1 < size) dst[k++] = *rep++; }
        else dst[k++] = *src;
    }
    dst[k] = '\0';
}

static int directory_index(int fd, const char *dir_path, const char *uri, int is_head) {
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;
    char names[4096][256];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < 4096) {
        if (entry->d_name[0] == '.') continue;
        snprintf(names[count], sizeof(names[count]), "%s", entry->d_name);
        count++;
    }
    closedir(dir);
    for (int i = 0; i < count - 1; i++) for (int j = i + 1; j < count; j++)
        if (strcmp(names[i], names[j]) > 0) { char tmp[256]; memcpy(tmp, names[i], sizeof(tmp)); memcpy(names[i], names[j], sizeof(names[i])); memcpy(names[j], tmp, sizeof(names[j])); }

    char *body = malloc(65536);
    if (!body) return -1;
    size_t len = 0;
    len += (size_t)snprintf(body + len, 65536 - len, "<!DOCTYPE html><html><head><title>Index of %s</title></head><body><h1>Index of %s</h1><ul>", uri, uri);
    for (int i = 0; i < count && len < 65536; i++) {
        char esc[512]; html_escape(esc, sizeof(esc), names[i]);
        len += (size_t)snprintf(body + len, 65536 - len, "<li><a href=\"%s\">%s</a></li>", esc, esc);
    }
    len += (size_t)snprintf(body + len, 65536 - len, "</ul></body></html>");
    char headers[1024];
    int hl = make_headers(headers, sizeof(headers), 200, "OK", "text/html; charset=UTF-8", (off_t)len, NULL, 1);
    if (hl > 0) send_all(fd, headers, (size_t)hl);
    if (!is_head) send_all(fd, body, len);
    free(body);
    return (int)len;
}

static char *peer_address(int fd) {
    static char buf[INET6_ADDRSTRLEN]; struct sockaddr_storage sa; socklen_t sl = sizeof(sa);
    getpeername(fd, (struct sockaddr *)&sa, &sl);
    if (sa.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, buf, sizeof(buf));
    else if (sa.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr, buf, sizeof(buf));
    else strcpy(buf, "-");
    return buf;
}

static void serve_cgi(int fd, const char *script, const char *path_info, const char *query) {
    int pipes[2];
    if (pipe(pipes) < 0) { send_error(fd, 500, "Internal Server Error", "Could not execute CGI."); return; }
    pid_t pid = fork();
    if (pid < 0) { close(pipes[0]); close(pipes[1]); send_error(fd, 500, "Internal Server Error", "Could not execute CGI."); return; }
    if (pid == 0) {
        close(pipes[0]); dup2(pipes[1], STDOUT_FILENO); close(pipes[1]);
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.0", 1);
        setenv("SERVER_SOFTWARE", VERSION, 1);
        setenv("REQUEST_METHOD", "GET", 1);
        setenv("PATH_INFO", path_info ? path_info : "", 1);
        setenv("QUERY_STRING", query ? query : "", 1);
        setenv("SCRIPT_NAME", script, 1);
        execl(script, script, (char *)NULL);
        _exit(1);
    }
    close(pipes[1]);
    char output[65536]; size_t used = 0;
    ssize_t n;
    while ((n = read(pipes[0], output + used, sizeof(output) - 1 - used)) > 0) { used += (size_t)n; if (used >= sizeof(output)-1) break; }
    close(pipes[0]); waitpid(pid, NULL, 0); output[used] = '\0';
    char *header_end = strstr(output, "\r\n\r\n"); size_t marker = 4;
    if (!header_end) { header_end = strstr(output, "\n\n"); marker = 2; }
    if (!header_end) {
        send_error(fd, 500, "Internal Server Error", "CGI returned no valid headers."); return;
    }
    *header_end = '\0'; char *body = header_end + marker;
    int status = 200; char reason[64] = "OK"; char type[128] = "text/plain; charset=UTF-8";
    for (char *line = strtok(output, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncasecmp(line, "Status:", 7)) sscanf(line + 7, "%d %63[^\r\n]", &status, reason);
        if (!strncasecmp(line, "Content-Type:", 13)) {
            snprintf(type, sizeof(type), "%s", line + 13);
            char *value = type; while (*value == ' ') value++;
            memmove(type, value, strlen(value) + 1);
        }
    }
    size_t body_len = strlen(body); char headers[1024];
    int hl = make_headers(headers, sizeof(headers), status, reason, type, (off_t)body_len, NULL, 1);
    if (hl > 0) send_all(fd, headers, (size_t)hl);
    send_all(fd, body, body_len);
}

static void handle_client(int fd, const char *remote) {
    char request[BUFFER_SIZE];
    if (read_request(fd, request, sizeof(request)) != 0) return;
    char *line_end = strstr(request, "\r\n"); if (!line_end) line_end = strchr(request, '\n');
    char request_line[4096] = ""; if (line_end) { size_t n=(size_t)(line_end-request); if(n>=sizeof(request_line))n=sizeof(request_line)-1; memcpy(request_line,request,n); request_line[n]='\0'; } else { size_t n=strlen(request); if(n>=sizeof(request_line))n=sizeof(request_line)-1; memcpy(request_line,request,n); request_line[n]='\0'; }
    char method[16], uri[4096], version[16];
    if (sscanf(request_line, "%15s %4095s %15s", method, uri, version) != 3) { send_error(fd,400,"Bad Request","Invalid request line."); log_request(remote,request_line,400,50); return; }
    if (strcmp(version,"HTTP/1.0") && strcmp(version,"HTTP/1.1") && strcmp(version,"HTTP/0.9")) { send_error(fd,400,"Bad Request","Unsupported HTTP version."); log_request(remote,request_line,400,50); return; }
    int is_head = strcmp(method,"HEAD") == 0; if (strcmp(method,"GET") && !is_head) { send_error(fd,501,"Not Implemented","Method not implemented."); log_request(remote,request_line,501,50); return; }
    if (uri[0]!='/' || strstr(uri,"//") || strstr(uri,"..") || strchr(uri,'\\')) { send_error(fd,400,"Bad Request","Invalid URI."); log_request(remote,request_line,400,50); return; }

    char *query=strchr(uri,'?'); if(query){*query='\0'; query++;} else query=NULL;
    if (!strncmp(uri,"/cgi-bin",8) && cgi_root) {
        const char *rest=uri+8; char script[4096]; snprintf(script,sizeof(script),"%s%s",cgi_root,rest && rest[0]=='/'?rest:"/"); char *slash=strchr(script,'?'); if(slash)*slash='\0';
        struct stat st; if(stat(script,&st)==0 && S_ISREG(st.st_mode)){ serve_cgi(fd, script, rest ? rest : "", query ? query : ""); log_request(remote,request_line,200,(long long)0); return; }
        send_error(fd,404,"Not Found","CGI script not found."); log_request(remote,request_line,404,50); return;
    }

    char resolved[4096];
    if (!strncmp(uri,"/~",2)) {
        char user[256], *slash=strchr(uri+2,'/'); size_t n=slash? (size_t)(slash-uri-2):strlen(uri+2); if(!n || n>=sizeof(user)){send_error(fd,400,"Bad Request","Invalid user URI.");return;}
        memcpy(user,uri+2,n); user[n]='\0'; struct passwd *pw=getpwnam(user); if(!pw){send_error(fd,404,"Not Found","User not found.");return;}
        snprintf(resolved,sizeof(resolved),"%s/sws%s",pw->pw_dir,slash?slash:"/");
    } else snprintf(resolved,sizeof(resolved),"%s%s",document_root,uri);

    struct stat st;
    if (stat(resolved,&st)!=0) { send_error(fd,404,"Not Found","The requested resource was not found."); log_request(remote,request_line,404,50); return; }
    if (S_ISDIR(st.st_mode)) {
        char indexpath[4096]; int ip_len=snprintf(indexpath,sizeof(indexpath),"%s/index.html",resolved);
        if(ip_len<0||(size_t)ip_len>=sizeof(indexpath)){send_error(fd,500,"Internal Server Error","Path too long.");return;}
        if(stat(indexpath,&st)==0&&S_ISREG(st.st_mode)){size_t resolved_len=strlen(indexpath);if(resolved_len>=sizeof(resolved)){send_error(fd,500,"Internal Server Error","Path too long.");return;}memcpy(resolved,indexpath,resolved_len+1);}else{int bytes=directory_index(fd,resolved,uri,is_head);log_request(remote,request_line,200,is_head?0:(long long)(bytes>0?bytes:0));return;}
    }
    if (!S_ISREG(st.st_mode)) { send_error(fd,403,"Forbidden","Access forbidden."); log_request(remote,request_line,403,50); return; }

    const char *ims=header_value(request,"If-Modified-Since"); if(ims){char since_text[128];snprintf(since_text,sizeof(since_text),"%s",ims);char *end=strpbrk(since_text,"\r\n");if(end)*end='\0';char *trim=since_text+strlen(since_text);while(trim>since_text&&(trim[-1]==' '||trim[-1]=='\t'))*--trim='\0';time_t since;if(parse_modified(since_text,&since)==0&&st.st_mtime<=since){char h[1024];int hl=make_headers(h,sizeof(h),304,"Not Modified",content_type(resolved),0,&st,1);if(hl>0)send_all(fd,h,(size_t)hl);log_request(remote,request_line,304,0);return;}}

    int file=open(resolved,O_RDONLY); if(file<0){send_error(fd,403,"Forbidden","Access forbidden.");log_request(remote,request_line,403,50);return;}
    char headers[1024]; int hl=make_headers(headers,sizeof(headers),200,"OK",content_type(resolved),st.st_size,&st,1); if(hl<0||hl>=(int)sizeof(headers)){close(file);return;} send_all(fd,headers,(size_t)hl);
    long long sent=0;
    if(!is_head){char buf[BUFFER_SIZE];ssize_t n;while((n=read(file,buf,sizeof(buf)))>0){if(send_all(fd,buf,(size_t)n)<0)break;sent+=(long long)n;}}
    close(file); log_request(remote,request_line,200,is_head?st.st_size:sent);
}

int main(int argc,char **argv) {
    char optstring[]="c:dhi:l:p:"; int opt; const char *cgi=NULL,*address=NULL,*logname=NULL,*port="8080";
    while((opt=getopt(argc,argv,optstring))!=-1){switch(opt){case 'c':cgi=optarg;break;case 'd':debug_mode=1;break;case 'h':usage(argv[0]);return 0;case 'i':address=optarg;break;case 'l':logname=optarg;break;case 'p':port=optarg;break;default:usage(argv[0]);return 1;}}
    if(optind>=argc){usage(argv[0]);return 1;} document_root=realpath(argv[optind],NULL); if(!document_root){fprintf(stderr,"sws: invalid document root: %s\n",argv[optind]);return 1;}
    struct stat ds; if(stat(document_root,&ds)||!S_ISDIR(ds.st_mode)){fprintf(stderr,"sws: document root is not a directory\n");return 1;}
    if(cgi){cgi_root=realpath(cgi,NULL);if(!cgi_root){fprintf(stderr,"sws: invalid CGI directory\n");return 1;}}
    if(logname){log_file=fopen(logname,"a");if(!log_file){perror("sws: log");return 1;}}

    struct addrinfo hints,*res,*p; memset(&hints,0,sizeof(hints));hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=address?0:AI_PASSIVE;
    if(getaddrinfo(address,port,&hints,&res)!=0){fprintf(stderr,"sws: invalid address or port\n");return 1;}
    int server=-1; for(p=res;p;p=p->ai_next){server=socket(p->ai_family,p->ai_socktype,p->ai_protocol);if(server<0)continue;int one=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));if(bind(server,p->ai_addr,p->ai_addrlen)==0)break;close(server);server=-1;} freeaddrinfo(res); if(server<0){fprintf(stderr,"sws: cannot bind to %s:%s\n",address?address:"all",port);return 1;} if(listen(server,128)<0){perror("listen");return 1;} signal(SIGCHLD,sigchld_handler);
    if(!debug_mode){pid_t pid=fork();if(pid<0)return 1;if(pid>0)return 0;if(setsid()<0)return 1;pid=fork();if(pid<0)return 1;if(pid>0)return 0;if(chdir("/")<0)return 1;int dn=open("/dev/null",O_RDWR);if(dn>=0){dup2(dn,0);dup2(dn,1);dup2(dn,2);if(dn>2)close(dn);}}
    for(;;){int client=accept(server,NULL,NULL);if(client<0){if(errno==EINTR)continue;continue;}if(debug_mode){handle_client(client,peer_address(client));close(client);}else{pid_t pid=fork();if(pid==0){close(server);handle_client(client,peer_address(client));close(client);_exit(0);}close(client);}}
}
