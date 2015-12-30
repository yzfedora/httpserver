#define USE_URL_DECODING 1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>		/* htons() or htonl() etc. */
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>		/* for macros of {PATH | NAME}_MAX */
#include <signal.h>

#include "thread_pool.h"


#define HTTP_VERSION	"HTTP/1.0"
#define SERV_VERSION	"webserver/1.0"

#define BACKLOG		20
#define BUFSZ		4096

#define METHOD_BUFSZ	32
#define VERSION_BUFSZ	32
#define PATHNAME_BUFSZ	(PATH_MAX + NAME_MAX)

#define FILESIZE_BUFSZ	128		/* buffer size of file size string */
#define DATE_BUFSZ	128		/* buffer size of date string */
#define HEADER_BUFSZ	512		/* HTTP response header */
#define HTMLHEADER_BUFSZ 1024		/* html header surround the entities */
#define ENTITY_BUFSZ	512		/* one entity size */
#define CONTENTS_BUFSZ	65535		/* all of the entities buffer size */
#define RESPONSE_BUFSZ	(HEADER_BUFSZ + CONTENTS_BUFSZ)

#define RFC1123FMT	"%a, %d %b %Y %H:%M:%S GMT"

#define SKIP_BLANK(start, end)						\
	do {								\
		while (isblank(*start) && start < end) start++;		\
	} while (0)



/* Some fixed responses format for client. */
#define HTTP_BAD_REQ_BODY						\
	"<HTML>"							\
		"<HEAD><TITLE>400 Bad Request</TITLE></HEAD>"		\
		"<BODY>"						\
			"<H4>400 Bad request</H4>"			\
				"Bad Request."				\
		"</BODY>"						\
	"</HTML>"

#define HTTP_NOT_SUPPORTED						\
	"<HTML>"							\
		"<HEAD><TITLE>501 Not supported</TITLE></HEAD>"		\
		"<BODY><H4>501 Not supported</H4>"			\
			"Method is not supported"			\
		"</BODY>"						\
	"</HTML>"

#define HTTP_NOT_FOUND							\
	"<HTML>"							\
		"<HEAD><TITLE>404 Not Found</TITLE></HEAD>"		\
		"<BODY><H4>404 Not Found</H4>"				\
			"File not found."				\
		"</BODY>"						\
	"</HTML>"

#define HTTP_FOUND							\
	"<HTML>"							\
		"<HEAD><TITLE>302 Found</TITLE></HEAD>"			\
		"<BODY><H4>302 Found</H4>"				\
			"Directories must end with a slash."		\
		"</BODY>"						\
	"</HTML>"

#define HTTP_FORBIDDEN							\
	"<HTML>"							\
		"<HEAD><TITLE>403 Forbidden</TITLE></HEAD>"		\
		"<BODY><H4>403 Forbidden</H4>"				\
			"Access denied."				\
		"</BODY>"						\
	"</HTML>"

#define HTTP_DIR_ITEMS							\
	"<tr><td><A HREF=\"%s\">%s</A></td><td>%s</td><td>%s</td></tr>"

#define HTTP_DIR_CONTENTS						\
	"<HTML>"							\
		"<HEAD><TITLE>Index of %s</TITLE></HEAD>"		\
		"<BODY>""<H4>Index of %s</H4>"				\
		"<table CELLSPACING=8>"					\
		"<tr>"							\
		"<th>Name</th><th>Last Modified</th><th>Size</th>"	\
		"</tr>"							\
		"%s"							\
		"</table><HR>"						\
		"<ADDRESS>%s</ADDRESS>"					\
		"</BODY>"						\
	"</HTML>"



/*
 * Prevent the partial sent when sending  large file or contents of a directory.
 */
static ssize_t nwrite(int fd, const void *buf, size_t count)
{
	int nwrt;
	const char *ptr = buf;
	size_t nleft = count;

	errno = 0;
	while (nleft > 0) {
		if ((nwrt = write(fd, ptr, nleft)) > 0) {
			nleft -= nwrt;
			ptr += nwrt;
		} else if (nwrt == 0) {
			fprintf(stderr, "connection has been closed.\n");
			return 0;
		} else {
			if (errno == EINTR)
				continue;
			perror("write");
			return -1;
		}
	}
	return count;
}

#if defined(USE_URL_DECODING)
/*
 * Used to encoding the Path, normally, the browser will encoding some special
 * characters to a '%' + 'hexdecimal value in ascii table'.
 * use '#define USE_URL_DECODING 0' to disable it.
 */
static char *pathname_decoding(char *pathname)
{
	char copy[PATHNAME_BUFSZ];
	int i, j, k, t;

	strncpy(copy, pathname, sizeof(copy));
	for (i = j = 0; copy[i]; i++, j++) {
		if (copy[i] == '%') {
			/* following 2 bytes is hex-decimal of a char */
			t = copy[i + 3];
			copy[i + 3] = 0;

			k = strtol(copy + i + 1, NULL, 16);
			pathname[j] = (char)k;

			copy[i + 3] = t;
			i += 2;
		} else {
			pathname[j] = copy[i];
		}
	}
	pathname[j] = 0;

	return pathname;
}
#endif

/*
 * Store current time to the buffer 'str', which returned format is accoding to
 * the RFC 1123. used in response header.
 */
static char *get_current_date(char *str, int len)
{
	time_t t = time(NULL);
	struct tm res;

	strftime(str, len , RFC1123FMT, gmtime_r(&t, &res));
	return str;
}

static char *get_filename(char *pathname)
{
	char *ptr = strrchr(pathname, '/');
	return (ptr ? ptr + 1 : pathname);
}

static char *get_date(time_t t, char *str, int len)
{	struct tm res;

	strftime(str, len , RFC1123FMT, gmtime_r(&t, &res));
	return str;
}

static int pathname_is_exist(const char *pathname)
{
	struct stat st;

	if (stat(pathname, &st) == -1 && errno == ENOENT)
		return 0;
	return 1;
}

static int pathname_is_directory(const char *pathname)
{
	struct stat st;

	if (stat(pathname, &st) == -1)
		perror("stat error when checking the type of pathname");

	if ((st.st_mode & S_IFMT) == S_IFDIR)
		return 1;
	return 0;

}
/*
 * Notes: we exit and terminate the program simply, when error occur. Normally,
 * in the large project, this is not a good idea.
 */

static int create_listen_sk(short port)
{
	int onoff = 1;
	int sk = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr = { 0 };

	if (sk == -1) {
		perror("create socket error");
		goto out;
	}

	/* set SO_REUSEADDR flags for us to test. */
	if (setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &onoff, sizeof(onoff)) < 0)
		perror("unable to set SO_REUSEADDR flags on socket");

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sk, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("bind error");
		goto out;
	}

	if (listen(sk, BACKLOG) == -1) {
		perror("listen error");
		goto out;
	}

	return sk;
out:
	if (sk != -1)
		close(sk);
	return -1;
}

static void response_bad_request(int sk)
{
	char date[DATE_BUFSZ];
	char buf[RESPONSE_BUFSZ];
	int len;

	len = snprintf(buf, sizeof(buf),
			  "%s 400 Bad Request\r\n"
			  "Server: %s\r\n"
			  "Date: %s\r\n"
			  "Content-Type: text/html\r\n"
			  "Content-Length: %ld\r\n"
			  "Connection: close\r\n\r\n"
			  "%s", HTTP_VERSION, SERV_VERSION,
			  get_current_date(date, sizeof(date)),
			  strlen(HTTP_BAD_REQ_BODY), HTTP_BAD_REQ_BODY);

	if (nwrite(sk, buf, len) <= 0)
		perror("nwrite error when response bad request");
}

static void response_not_supported(int sk)
{
	char date[DATE_BUFSZ];
	char buf[RESPONSE_BUFSZ];
	int len;
	
	len = snprintf(buf, sizeof(buf),
			  "%s 501 Not supported\r\n"
			  "Server: %s\r\n"
			  "Date: %s\r\n"
			  "Content-Type: text/html\r\b"
			  "Content-Length: %ld\r\n"
			  "Connection: close\r\n\r\n"
			  "%s", HTTP_VERSION, SERV_VERSION,
			  get_current_date(date, sizeof(date)),
			  strlen(HTTP_NOT_SUPPORTED), HTTP_NOT_SUPPORTED);
	
	if (nwrite(sk, buf, len) <= 0)
		perror("nwrite error when response not supported");
}

static void response_not_found(int sk)
{
	char date[DATE_BUFSZ];
	char buf[RESPONSE_BUFSZ];
	int len;
	
	len = snprintf(buf, sizeof(buf),
			  "%s 404 Not Found\r\n"
			  "Server: %s\r\n"
			  "Date: %s\r\n"
			  "Content-Type: text/html\r\b"
			  "Content-Length: %ld\r\n"
			  "Connection: close\r\n\r\n"
			  "%s", HTTP_VERSION, SERV_VERSION,
			  get_current_date(date, sizeof(date)),
			  strlen(HTTP_NOT_FOUND), HTTP_NOT_FOUND);
	
	if (nwrite(sk, buf, len) <= 0)
		perror("nwrite error when response request path not found");
}

static void response_found(int sk, const char *pathname)
{
	char date[DATE_BUFSZ];
	char buf[RESPONSE_BUFSZ];
	int len;
	
	len = snprintf(buf, sizeof(buf),
			  "%s 302 Found\r\n"
			  "Server: %s\r\n"
			  "Date: %s\r\n"
			  "Location: %s\\\r\n"
			  "Content-Type: text/html\r\n"
			  "Content-Length: %ld\r\n"
			  "Connection: close\r\n\r\n"
			  "%s", HTTP_VERSION, SERV_VERSION,
			  get_current_date(date, sizeof(date)), pathname,
			  strlen(HTTP_FOUND), HTTP_FOUND);
	
	if (nwrite(sk, buf, len) <= 0)
		perror("nwrite error when response request resource found in"
		       "other place");
}

static void response_forbidden(int clisk)
{
	char date[DATE_BUFSZ];
	char buf[RESPONSE_BUFSZ];
	int len;
	
	len = snprintf(buf, sizeof(buf),
			  "%s 403 Forbidden\r\n"
			  "Server: %s\r\n"
			  "Date: %s\r\n"
			  "Content-Type: text/html\r\n"
			  "Content-Length: %ld\r\n"
			  "Connection: close\r\n\r\n"
			  "%s", HTTP_VERSION, SERV_VERSION,
			  get_current_date(date, sizeof(date)),
			  strlen(HTTP_FORBIDDEN), HTTP_FORBIDDEN);
	
	if (nwrite(clisk, buf, len) <= 0)
		perror("nwrite error when response request forbidden");

}


static char *get_mime_type(const char *name)
{
	char *ext = strrchr(name, '.');

	if (!ext)
		return NULL;

	if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
		return "text/html";
	if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
		return "image/jpeg";
	if (strcmp(ext, ".gif") == 0)
		return "image/gif";
	if (strcmp(ext, ".png") == 0)
		return "image/png";
	if (strcmp(ext, ".css") == 0)
		return "text/css";
	if (strcmp(ext, ".au") == 0)
		return "audio/basic";
	if (strcmp(ext, ".wav") == 0)
		return "audio/wav";
	if (strcmp(ext, ".avi") == 0)
		return "video/x-msvideo";
	if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0)
		return "video/mpeg";
	if (strcmp(ext, ".mp3") == 0)
		return "audio/mpeg";

	return NULL;
}

/*
 * We parsing the METHOD, PATH and VERSION tokens from the request. return 0 on
 * success, if the request from client couldn't be understood, then -1 will
 * be returned.
 */
static int parsing_request_header(char *data, char *method, int method_len,
				  char *pathname, int pathname_len,
				  char *version, int version_len)
{
	int i;
	char *startptr = data;
	char *endptr = strstr(data, "\r\n");

	if (!endptr)
		return -1;
	
	*endptr = 0;

	i = 0;
	SKIP_BLANK(startptr, endptr);
	while (startptr < endptr && i < method_len && !isblank(*startptr)) {
		method[i++] = *startptr++;
	}
	method[i] = 0;

	i = 0;
	SKIP_BLANK(startptr, endptr);	
	while (startptr < endptr && i < pathname_len && !isblank(*startptr)) {
		pathname[i++] = *startptr++;
	}
	pathname[i] = 0;
	
	i = 0;
	SKIP_BLANK(startptr, endptr);	
	while (startptr < endptr && i < version_len && !isblank(*startptr)) {
		version[i++] = *startptr++;
	}
	version[i] = 0;

	/* A regular HTTP request, should at least have these 3 tokens. */
	if (!*method || !*pathname || !*version)
		return -1;

	return 0;
}

static char *pathname_find_file(char *pathname, const char *file)
{
	DIR *dir = opendir(pathname);
	struct dirent entry, *res;

	if (!dir)
		goto out;

	while (readdir_r(dir, &entry, &res) == 0 && res) {
		if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, ".."))
			continue;

		if (!strcmp(entry.d_name, file))
			return strncat(pathname, file, PATHNAME_BUFSZ);

	}
out:
	return NULL;
}

static int transfer_header(int clisk, const char *pathname,
			   size_t content_length)
{
	char date[DATE_BUFSZ];
	char buf[HEADER_BUFSZ];
	int len;
	char *mime = get_mime_type(pathname);

	if (!mime) {
		fprintf(stderr, "unknown mime type when request file: %s.\n",
				pathname);
		return -1;
	}
	
	len = snprintf(buf, sizeof(buf),
			  "%s 200 OK\r\n"
			  "Server: %s\r\n"
			  "Date: %s\r\n"
			  "Content-Type: %s\r\n"
			  "Content-Length: %ld\r\n"
			  "Connection: close\r\n\r\n",
			  HTTP_VERSION, SERV_VERSION,
			  get_current_date(date, sizeof(date)),
			  mime, content_length);
	
	if (nwrite(clisk, buf, len) <= 0) {
		perror("nwrite error when transfer http header to client");
		return -1;
	}

	return 0;
}

static off_t get_file_length_by_fd(int fd)
{
	size_t save, length;

	if ((save = lseek(fd, 0, SEEK_CUR)) == (off_t)-1)
		return -1;
	if ((length = lseek(fd, 0, SEEK_END)) == (off_t)-1)
		return -1;
	if ((lseek(fd, save, SEEK_SET)) == (off_t)-1)
		return -1;

	return length;
}

static int transfer_file(int clisk, const char *pathname)
{
	int fd;
	off_t length;
	ssize_t nread;
	char buf[BUFSZ];

	if ((fd = open(pathname, O_RDONLY)) == -1) {
		perror("open error when transfer file");
		goto out;
	}

	if ((length = get_file_length_by_fd(fd)) == -1) {
		perror("couldn't get the length of requested file");
		goto out;
	}

	/*
	 * if the file of client requested is a not valid MIME type, terminate
	 * this transfer.
	 */
	if (transfer_header(clisk, pathname, length) == -1)
		goto out;

	while ((nread = read(fd, buf, sizeof(buf))) > 0) {
		if (nwrite(clisk, buf, nread) <= 0) {
			fprintf(stderr, "nwrite error when transfer file.\n");
			goto out;
		}
	}

	return 0;
out:
	return -1;
}

/*
 * we allocate a new space enough to store the HTML header and the contents
 * which consists with entities in the directory. if success, the contents
 * will include the HTML header, and return 0. otherwise, -1 will returned,
 * and the nothing will be changed.
 */
static int add_html_header_to_contents(char **contents, size_t *contents_len,
				       size_t *offset, char *pathname)
{
	char *ptr = malloc(*contents_len + HTMLHEADER_BUFSZ);
	if (!ptr) {
		perror("allocate memory error when add html header"
						"to the contents");
		return -1;
	}


	*offset = snprintf(ptr, *contents_len + HTMLHEADER_BUFSZ,
			   HTTP_DIR_CONTENTS, pathname, pathname,
			   *contents, SERV_VERSION);
	if (*contents)
		free(*contents);
	
	*contents = ptr;
	*contents_len += HTMLHEADER_BUFSZ;

	return 0;
}

static int transfer_dir_contents(int clisk, char **contents,
				 size_t *contents_len, size_t *offset,
				 char *pathname)
{
	int ret = -1;
	char date[DATE_BUFSZ];
	char *buf = NULL;
	size_t len;

	if (add_html_header_to_contents(contents, contents_len, offset,
					pathname) == -1)
		goto out;

	if (!(buf = malloc(*contents_len + HEADER_BUFSZ))) {
		perror("allocate memory error when transfer contents of "
							"directory");
		goto out;
	}
	
	len = snprintf(buf, *contents_len + HEADER_BUFSZ,
		       "%s 200 OK\r\n"
		       "Server: %s\r\n"
		       "Date: %s\r\n"
		       "Content-Type: text/html\r\n"
		       "Content-Length: %ld\r\n"
		       "Connection: close\r\n\r\n"
		       "%s", HTTP_VERSION, SERV_VERSION,
		       get_current_date(date, sizeof(date)),
		       *contents_len, *contents);

	if (nwrite(clisk, buf, len) <= 0) {
		perror("nwrite error when send contents of directory");
		goto out;
	}

	ret = 0;
out:
	if (buf)
		free(buf);
	return ret;	
}

/*
 * Notes: this functions will only add most 'contents_len' bytes to the buffer
 * 'contents'(including the terminating '0'). This will cause if the files
 * under the directory is too many, there may not be enough space to store it.
 *
 * Alternatively, We just pass a pointer as the 'contents', and the memory will
 * be allocated by itself, if no enough space there are, it will re-allocate it.
 */
static int dir_contents_add(char **contents, size_t *contents_len,
			    size_t *offset, char *pathname)
{
	char date_str[DATE_BUFSZ];
	char filesize_str[FILESIZE_BUFSZ] = "";
	char *filename = get_filename(pathname);
	struct stat st;

	if (!*contents) {
		/* First time, we need to allocate a memory for us to use. */
		if (!(*contents = malloc(CONTENTS_BUFSZ))) {
			perror("allocate memory error when add entity "
							"of directory");
			goto out;
		}
	
		*contents_len = CONTENTS_BUFSZ;
	}


	if ((*contents_len - *offset) <= ENTITY_BUFSZ) {
		/* 
		 * no enough space to store next HTML formatted file name
		 * entity. use realloc() to extension the space.
		 */
		char *ptr = realloc(*contents, *contents_len + CONTENTS_BUFSZ);
		if (!ptr) {
			perror("re-allocate memory error when add entity "
							"of directory");
			goto out;
		}

		*contents = ptr;
		*contents_len += CONTENTS_BUFSZ;
	}


	/* If couldn't stat the pathname, we also return 0. only allocate the
	 * memory error was occur. -1 will be returned. */
	if (stat(pathname, &st) == -1) {
		perror("stat error when try add entity of directory.");
		errno = 0;
		return 0;
	}

	if (S_ISREG(st.st_mode))
		snprintf(filesize_str, sizeof(filesize_str),
					"%ld bytes", st.st_size);
	else if (S_ISDIR(st.st_mode))
		strncat(filename, "/", PATHNAME_BUFSZ - strlen(pathname));

	get_date((time_t)st.st_mtime, date_str, sizeof(date_str));
	*offset += snprintf(*contents + *offset, *contents_len - *offset,
			    HTTP_DIR_ITEMS, filename, filename,
			    date_str, filesize_str);

	return 0;
out:
	return -1;
}

/*
 * When request directory haven't index.html, we travel the directory, and
 * return a html file which contains a file list of current directory.
 */
static int transfer_list(int clisk, char *pathname)
{
	int ret = -1;
	size_t pathname_len = strlen(pathname);
	char *ptr = pathname + pathname_len;
	
	char *contents = NULL;		/* must initial */
	size_t contents_len = 0;
	size_t offset = 0;		/* used record offset, more							   efficency */
	DIR *dir = opendir(pathname);
	struct dirent entry, *res;
	

	if (!dir) {
		if (errno == EACCES)
			response_forbidden(clisk);
		else
			perror("opendir error when transfer list of file");

		goto out;
	}
	
	while (readdir_r(dir, &entry, &res) == 0 && res) {
		strncpy(ptr, entry.d_name, PATHNAME_BUFSZ - pathname_len);
	
		/*
		 * Add the file's name, modification time and size to a buffer
		 * in form of HTML.
		 */	
		if (dir_contents_add(&contents, &contents_len, &offset,
				     pathname) == -1 ) {
			goto out;
		}
		*ptr = 0;
	}

	if (transfer_dir_contents(clisk, &contents, &contents_len,
					 &offset, pathname) == -1)
		goto out;

	ret = 0;
out:
	if (contents)
		free(contents);
	return ret;
}


static int process_pathname_is_directory(int clisk, char *pathname)
{
	char *index_file = pathname_find_file(pathname, "index.html");

	if (index_file) {
		if (transfer_file(clisk, index_file) == -1)
			return -1;
	}
	
	/*
	 * not 'index.html', then return a file list of current dir.
	 */
	if (transfer_list(clisk, pathname) == -1)
		return -1;

	return 0;
}

static int pathname_is_file(const char *pathname)
{
	struct stat st;
	
	if (stat(pathname, &st) == -1)
		return 0;

	if (S_ISREG(st.st_mode))
		return 1;

	return 0;
}

static int has_permission_to_read(const char *pathname)
{
	int fd;

	if ((fd = open(pathname, O_RDONLY)) >= 0) {
		close(fd);
		return 1;
	}

	return 0;
}

/*
 * This function will be passing to the thread pool, used to process the request
 * from the client, it will be store into a work_t object with socket descriptor
 * associated with client. and finally will be called from a free thread.
 */
static int process_request(void *arg)
{
	int ret = -1;
	int clisk = *((int *)arg);
	char pathname[PATHNAME_BUFSZ];
	char method[METHOD_BUFSZ];
	char version[VERSION_BUFSZ];
	char buf[BUFSZ];
	size_t nread;

	free(arg);

	if ((nread = read(clisk, buf, sizeof(buf))) == -1) {
		perror("read request from client");
		goto out;
	}

	if (parsing_request_header(buf, method, sizeof(method),
				   pathname, sizeof(pathname),
				   version, sizeof(version)) == -1) {
		response_bad_request(clisk);
		goto out;
	}


	if (strcmp(method, "GET")) {		/* no supported method */
		response_not_supported(clisk);
		goto out;
	}

#if defined(USE_URL_DECODING)
	pathname_decoding(pathname);
#endif

	if (!pathname_is_exist(pathname)) {	/* pathname don't exist */
		response_not_found(clisk);
		goto out;
	}
	
	if (pathname_is_directory(pathname)) {
		if (pathname[strlen(pathname) - 1] != '/') {
			response_found(clisk, pathname);
			goto out;
		}
		
		process_pathname_is_directory(clisk, pathname);
		ret = 0;
		goto out;
	}

	if (!pathname_is_file(pathname) || !has_permission_to_read(pathname)) {
		response_forbidden(clisk);
		goto out;
	}
	
	if (transfer_file(clisk, pathname) == -1)
			goto out;


out:
	close(clisk);
	return ret;
		
}

static int server_launch(int port, int pool_size, int max_request)
{
	int sk = -1;
	int *skptr;
	int request_counter = 0;
	struct thread_pool *pool;

	if ((sk = create_listen_sk(port)) == -1)
		goto out;

	if (!(pool = thread_pool_new(pool_size))) {
		fprintf(stderr, "create the thread pool failure.\n");
		goto out;
	}

	while (request_counter < max_request) {
		if (!(skptr = malloc(sizeof(*skptr)))) {
			perror("allocate memory to store the descriptor error");
			continue;
		}

		if ((*skptr = accept(sk, NULL, 0)) == -1) {
			perror("accept");
			continue;
		}

		dispatch(pool, process_request, skptr);
		request_counter++;
	}

	thread_pool_delete(pool);

	return 0;
out:
	if (sk != -1)
		close(sk);
	return -1;
}

static int ignore_sigpipe(void)
{
	struct sigaction act = { 0 };

	act.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &act, NULL) == -1) {
		perror("ignore_sigpipe");
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{

	if (argc != 4) {
		fprintf(stderr, "Usage: server <port> <pool-size> "
			"<max-number-of-request>\n");
		exit(EXIT_FAILURE);
	}

	/* Ignore the SIGPIPE, it will cause server terminate unexpectedly, when
	 * you write the data to client somtimes. */
	if (ignore_sigpipe() == -1)
		return -1;

	if (server_launch(atoi(argv[1]), atoi(argv[2]), atoi(argv[3])) == -1)
		return -1;

	return 0;
}
