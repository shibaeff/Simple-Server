/* 
 * Простой сервер.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUFSIZE 1024
#define MAXERRS 16

int is_verbose = 1;
extern char **environ; /* контекст окружения */

/*
 * обертка для обработки ошибок
 */
void error(char *msg) {
  perror(msg);
  exit(1);
}

/*
 * сообщение об ошибке
 */
void cerror(FILE *stream, char *cause, char *errno, 
	    char *shortmsg, char *longmsg) {
  fprintf(stream, "HTTP/1.1 %s %s\n", errno, shortmsg);
  fprintf(stream, "Content-type: text/html\n");
  fprintf(stream, "\n");
  fprintf(stream, "<html><title>Stupid Error</title>");
  fprintf(stream, "<body bgcolor=""ffffff"">\n");
  fprintf(stream, "%s: %s\n", errno, shortmsg);
  fprintf(stream, "<p>%s: %s\n", longmsg, cause);
  fprintf(stream, "<hr><em>The Stupid Web server</em>\n");
}

int main(int argc, char **argv) {

  /* переменные, отвечающие за соединение*/
  int parentfd;          /* родительский сокет */
  int childfd;           /* сокет-потомок */
  int portno;            /* порт прослушки */
  int clientlen;         /* длина клиентского адреса*/
  struct hostent *hostp; /* client host info */
  char *hostaddrp;       /* dotted decimal host addr string */
  int optval;            /* flag value for setsockopt */
  struct sockaddr_in serveraddr; /* адрес сервера */
  struct sockaddr_in clientaddr; /* адрес клиента */

  /* соединение, I/O */
  FILE *stream;          /* поток для childfd */
  char buf[BUFSIZE];     /* буффер собщений */
  char method[BUFSIZE];  /* request method */
  char uri[BUFSIZE];     /* request uri */
  char version[BUFSIZE]; /* request method */
  char filename[BUFSIZE];/* path derived from uri */
  char filetype[BUFSIZE];/* path derived from uri */
  char cgiargs[BUFSIZE]; /* cgi argument list */
  char *p;               /* временный указатель */
  int is_static;         /* запрос статический? */
  struct stat sbuf;      /* статус файла */
  int fd;                /* дескриптор для статического контента */
  int pid;               /* process id from fork */
  int wait_status;       /* статус от wait */

  /* check command line args */
  if (argc < 2) {
    fprintf(stderr, "Укажите номер порта: %s <port>\n", argv[0]);
    exit(1);
  }
  portno = atoi(argv[1]);
  
  if (argc >= 3) {
 	is_verbose = 1;
	printf("Running in verbmode\n");
  } else {
	is_verbose = 0;
        printf("Running silently\n");
  }
  /* open socket descriptor */
  parentfd = socket(AF_INET, SOCK_STREAM, 0);
  if (parentfd < 0) 
    error("Не удается открыть сокет");

  /* allows us to restart server immediately */
  optval = 1;
  setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, 
	     (const void *)&optval , sizeof(int));

  /* bind port to socket */
  bzero((char *) &serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons((unsigned short)portno);
  if (bind(parentfd, (struct sockaddr *) &serveraddr, 
	   sizeof(serveraddr)) < 0) 
    error("ERROR on binding");

  /* готовимся к получению запросов на соединение*/
  if (listen(parentfd, 5) < 0) /* до 5 запросов одновременно */
    error("ERROR on listen");

  /* 
   * main loop: ждем запроса соединения, парсим HTTP,
   * serve requested content, close connection.
   */
  clientlen = sizeof(clientaddr);
  while (1) {

    /* ждем запроса соединения */
    childfd = accept(parentfd, (struct sockaddr *) &clientaddr, &clientlen);
    if (childfd < 0) 
      error("Соединение не принято");
    
    /* узнаем отправителя */
    hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, 
			  sizeof(clientaddr.sin_addr.s_addr), AF_INET);
    if (hostp == NULL)
      error("Невозможно получить адрес отправителя");
    hostaddrp = inet_ntoa(clientaddr.sin_addr);
    if (hostaddrp == NULL)
      error("Ошибка конвертации\n");
    
    /* Открываем дескриптор как поток */
    if ((stream = fdopen(childfd, "r+")) == NULL)
      error("ERROR on fdopen");

    /* get the HTTP request line */
    fgets(buf, BUFSIZE, stream);
    if (is_verbose)
    	printf("%s", buf);
    sscanf(buf, "%s %s %s\n", method, uri, version);

    /* Сервер работает только с GET  */
    if (strcasecmp(method, "GET")) {
      cerror(stream, method, "501", "Not Implemented", 
	     "Server does not implement this method");
      fclose(stream);
      close(childfd);
      continue;
    }

    /* Читаем заголовки */
    fgets(buf, BUFSIZE, stream);
    printf("%s", buf);
    while(strcmp(buf, "\r\n")) {
      fgets(buf, BUFSIZE, stream);
      printf("%s", buf);
    }

    /* парсим uri [crufty] */
    if (!strstr(uri, "cgi-bin")) { /* статический контент */
      is_static = 1;
      strcpy(cgiargs, "");
      strcpy(filename, ".");
      strcat(filename, uri);
      if (uri[strlen(uri)-1] == '/') 
	strcat(filename, "index.html");
    }
    else { /* динамический контент */
      is_static = 0;
      p = index(uri, '?');
      if (p) {
	strcpy(cgiargs, p+1);
	*p = '\0';
      }
      else {
	strcpy(cgiargs, "");
      }
      strcpy(filename, ".");
      strcat(filename, uri);
    }

    /* проверяем, существует ли ресурс */
    if (stat(filename, &sbuf) < 0) {
      cerror(stream, filename, "404", "Not found", 
	     "Stupid couldn't find this file");
      fclose(stream);
      close(childfd);
      continue;
    }

    /* предоставляем статический контет */
    if (is_static) {
      if (strstr(filename, ".html"))
	strcpy(filetype, "text/html");
      else if (strstr(filename, ".gif"))
	strcpy(filetype, "image/gif");
      else if (strstr(filename, ".jpg"))
	strcpy(filetype, "image/jpg");
      else 
	strcpy(filetype, "text/plain");

      /* печатаем заголовки */
      fprintf(stream, "HTTP/1.1 200 OK\n");
      fprintf(stream, "Server: Stupid Web Server\n");
      fprintf(stream, "Content-length: %d\n", (int)sbuf.st_size);
      fprintf(stream, "Content-type: %s\n", filetype);
      fprintf(stream, "\r\n"); 
      fflush(stream);

      /* mmap для того, чтобы сгенерировать тело ответа */
      fd = open(filename, O_RDONLY);
      p = mmap(0, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      fwrite(p, 1, sbuf.st_size, stream);
      munmap(p, sbuf.st_size);
    }

    /* динамический контент */
    else {
      /* это обычный файл??? */
      if (!(S_IFREG & sbuf.st_mode) || !(S_IXUSR & sbuf.st_mode)) {
	cerror(stream, filename, "403", "Forbidden", 
	       "You are not allow to access this item");
	fclose(stream);
	close(childfd);
	continue;
      }

      /* задаем переменные окружения*/
      setenv("QUERY_STRING", cgiargs, 1); 

      /* первая часть ответа */
      sprintf(buf, "HTTP/1.1 200 OK\n");
      write(childfd, buf, strlen(buf));
      sprintf(buf, "Server: Stupid Web Server\n");
      write(childfd, buf, strlen(buf));

      /* запускаем CGI процесс, его вывод ловим через childfd*/
      pid = fork();
      if (pid < 0) {
	perror("ERROR in fork");
	exit(1);
      }
      else if (pid > 0) { /* родительский процесс*/
	wait(&wait_status);
      }
      else { /* дочерний*/
	close(0); /*закрываем ввод */
	dup2(childfd, 1); /* мапим stdout */
	dup2(childfd, 2); /* мапим stderr */
	if (execve(filename, NULL, environ) < 0) {
	  perror("ERROR in execve");
	}
      }
    }

    /* cочистка */
    fclose(stream);
    close(childfd);

  }
}
