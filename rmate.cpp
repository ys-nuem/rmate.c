extern "C" {
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
}

#include "version.h"


#define HOST_ENV "RMATE_HOST"
#define PORT_ENV "RMATE_PORT"
#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT "52698"

#define MAXDATASIZE 1024


int get_server_info(char const* host, char const* port, struct addrinfo** servinfo) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    return getaddrinfo(host, port, &hints, servinfo);
}

int make_tcp_connection(struct addrinfo* p) {
	// loop through all the results and connect to the first we can
	while (p != NULL) {
        int sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sockfd == -1) {
            p = p->ai_next;
    		continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
            p = p->ai_next;
			continue;
		}

		return sockfd;
	}

    return -1;
}

ssize_t readline(char* buf, size_t len) {
    char* cmd_str = static_cast<char*>(memchr(buf, '\n', len));
    if (!cmd_str)
        return -1;
    
    ssize_t line_len = cmd_str - buf;
    if (line_len > 0 && cmd_str[-1] == '\r')
        cmd_str[-1] = '\0';
    cmd_str[0] = '\0';
    
    return line_len + 1;
}

int connect_mate(const char* host, const char* port)
{
    // get address information of rmate server.
    struct addrinfo* servinfo;
    int ret = get_server_info(host, port, &servinfo);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

	int sockfd = make_tcp_connection(servinfo);

	freeaddrinfo(servinfo); // all done with this structure    
    return sockfd;
}

int send_open(int sockfd, const char* filename, int fd) {
    struct stat st;    
    if (fstat(fd, &st) == -1) {
        perror("stat");
        return -1;
    }

    char* fdata = static_cast<char*>(mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (fdata == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    char resolved[PATH_MAX];    
    dprintf(sockfd, "open\n");
    dprintf(sockfd, "display-name: %s\n", filename);
    dprintf(sockfd, "real-path: %s\n", realpath(filename, resolved));
    dprintf(sockfd, "data-on-save: yes\n");
    dprintf(sockfd, "re-activate: yes\n");
    dprintf(sockfd, "token: %s\n", filename);
    dprintf(sockfd, "data: %zd\n", st.st_size);
    write(sockfd, fdata, st.st_size);
    dprintf(sockfd, "\n.\n");
    
    munmap(fdata, st.st_size);
    return 0;
}

int receive_save(int sockfd, char* rem_buf, size_t rem_buf_len, const char* filename, size_t filesize) {
    int fd = open(filename, O_RDWR);
    if (fd == -1) {
        perror("open");
        return -1;
    }
    
    if (ftruncate(fd, filesize) == -1) {
        perror("ftruncate");
        return -1;
    }
    
    char* fdata = static_cast<char*>(mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (fdata == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    
    if (rem_buf_len > filesize)
        rem_buf_len = filesize;
    
    memcpy(fdata, rem_buf, rem_buf_len);
    int numbytes = read(sockfd, fdata + rem_buf_len, filesize - rem_buf_len);
    if (numbytes == -1) {
        perror("read");
        return -1;
    }
    
    if (munmap(fdata, filesize) == -1) {
        perror("munmap");
        return -1;
    }
        
    close(fd);
    return 0;
}

int read_command(int sockfd, char* buf, size_t len) {
    int numbytes = read(sockfd, buf, len - 1);
    if (numbytes == -1) {
        perror("read");
        return -1;
    }

    if (numbytes == 0)
        return 0;

    buf[numbytes] = '\0';
    return numbytes;
}

class RmateState {
    enum CMD_STATE {
        CMD_HEADER,
        CMD_CMD,
        CMD_VAR,
        CMD_END
    };

    enum CMD_TYPE {
        UNKNOWN,
        CLOSE,
        SAVE
    };

    CMD_STATE state = CMD_HEADER;
    CMD_TYPE type = UNKNOWN;
    char* filename = NULL;
    size_t file_len = 0;

public:
    ssize_t handle_cmds(int sockfd, char* buf, size_t len) {
        size_t total_read_len = 0;
        
        while (total_read_len < len) {
            ssize_t read_len = handle_line(sockfd, buf, len);
            if (read_len == -1)
                return -1;
            
            buf += read_len;
            total_read_len += read_len;
        }
        
        return total_read_len;
    }

private:
    void handle_var(const char* name, const char* value) {
        if (!strcmp(name, "token"))
            this->filename = strdup(value);
        
        if (!strcmp(name, "data"))
            this->file_len = strtoul(value, NULL, 10);
    }

    ssize_t handle_line(int sockfd, char* buf, size_t len) {
        ssize_t read_len = -1;
        size_t token_len;
        char *name, *value;
        
        switch(this->state) {
        case CMD_HEADER:
            if((read_len = readline(buf, len)) > 0) {
                this->state = CMD_CMD;
            }
            
            break;
        case CMD_CMD:
            if ((read_len = readline(buf, len)) > 0 && *buf != '\0') {
                free(this->filename);
                memset(this, 0, sizeof(RmateState));

                if (!strncmp(buf, "close", read_len))
                    this->type = CLOSE;
                
                if (!strncmp(buf, "save", read_len))
                    this->type = SAVE;
                
                this->state = CMD_VAR;
            }
            
            break;
        case CMD_VAR:
            if((read_len = readline(buf, len)) < 0) 
                goto err;
            
            if(*buf == '\0')
                goto err;
            
            if((token_len = strcspn(buf, ":")) >= (size_t) read_len)
                goto err;
                
            this->state = CMD_VAR;
            name = buf;
            name[token_len] = '\0';
            value = name + token_len + 1;
            value += strspn(value, " ");
        
            handle_var(name, value);
            if (!strcmp(name, "data"))
                receive_save(sockfd, buf + read_len, len - read_len, this->filename, this->file_len);
            break;
            
            err:
            this->state = CMD_CMD;
            break;
        default:
            break;
        }
        
        return read_len;
    }
};

void version() {
  char cc[256];
  time_t tc = COMMIT_DATE;

  strftime(cc, sizeof(cc), "%Y-%m-%d", localtime(&tc));
  printf("rmate %s (%s)\n", BUILD_VERSION, cc);
  printf("Copyright (c) 2014 Mael Clerambault\n");
  exit(0);
}

void usage() {
  fprintf(stderr, "Usage: rmate [options] file\n");
  fprintf(stderr, "\nOptions:\n");
  fprintf(stderr, "  -h\t\tPrint this help\n");
  fprintf(stderr, "  -v\t\tPrint version informations\n");
  fprintf(stderr, "  -H HOST\tConnect to host. Defaults to $%s or %s.\n", HOST_ENV, DEFAULT_HOST);
  fprintf(stderr, "  -p PORT\tPort number to use for connection. Defaults to $%s or %s.\n", PORT_ENV, DEFAULT_PORT);
  fprintf(stderr, "  -w\t\tWait for file to be closed by TextMate.\n");
  exit(0);
}

int main(int argc, char *argv[])
{
    signal(SIGCHLD, SIG_IGN);

    char* host = getenv(HOST_ENV);
    if (!host)
        host = DEFAULT_HOST;

    char* port = getenv(PORT_ENV);
    if (!port)
        port = DEFAULT_PORT;

    int need_wait = 0;

    int ch;
    while ((ch = getopt(argc, argv, "whvH:p:")) != -1) {
      switch(ch) {
        case 'w':
          need_wait = 1;
          break;
		case 'H':
          host = optarg;
          break;
    	case 'p':
          port = optarg;
          break;
        case 'v':
          version();
          break;
        case 'h':
        default:
          usage();
          break;
      }
    }
    argc -= optind; 
    argv += optind;
    
    if (argc < 1)
        usage();
    
    char* filename = argv[0];

    // run as background process.
    if (!need_wait) {
        if (fork() > 0)
            exit(0);
    }

    // create a connection to the server.
    {
        int sockfd = connect_mate(host, port);
        if (sockfd == -1) {
            fprintf(stderr, "Could not connect\n");
            return -1;
        }
    
        // send all contents of file to server.
        {
            int fd = open(filename, O_RDONLY);    
            if (fd == -1) {
                perror("open");
                return -1;
            }    
            send_open(sockfd, filename, fd);
            close(fd);
        }

        RmateState state;
        while (1) {
            char buf[MAXDATASIZE];
            int numbytes = read_command(sockfd, buf, MAXDATASIZE);
            if (numbytes < 0) {
                return numbytes;
            }

            state.handle_cmds(sockfd, buf, numbytes);
        }

        close(sockfd);
    }

	return 0;
}
