//
// Created by a on 1/11/17.
//
//http://www.linuxhowtos.org/data/6/server.c
#include "server.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "stdbool.h"
#include <dirent.h>
#include "processor.h"
#include "string_helper.h"
#include "etag_helper.h"
#include "cgi_bin.h"

/*
 * pushes 404 header
 */
static bool push404(struct processor_state *aux) {
  static const char *msg404 = "HTTP/1.1 404 Not Found\r\n"
          "Content-Type: text/html\r\n"
          "Content-Length: 207\r\n"
          "\r\n"
          "<html><head>\n"
          "<title>Error response</title>\n"
          "</head>\n"
          "<body>\n"
          "<h1>Error response</h1>\n"
          "<p>Error code 404.\n"
          "<p>Message: File not found.\n"
          "<p>Error code explanation: 404 = Nothing matches the given URI.\n"
          "</body></html>";

  int err = write(aux->fd, msg404, strlen(msg404));
  if (err >= 0) aux->log_data.sent_length += err;
  aux->log_data.status_code = 404;

  if (err < 0 || (unsigned) err != strlen(msg404)) {
    char str[100];
    sprintf(str, "Could not push whole message 404, pushed %d", err);
    log_write_error(&aux->log_data, str);
  }
}

/*
 * parses header for error, pushes 404 in case of the problem.
 */
static static bool was404_error(struct processor_state *aux, http_map_entry *http) {
  char *domain_port = strdup(http_get_val(http, "host"));
  char *domain = NULL;
  char *port = NULL;

  bool is404 = false;
  int i = 0;
  for (char *save_ptr = NULL, *token = strtok_r(domain_port, ":", &save_ptr); token != NULL;
       token = strtok_r(NULL, ":", &save_ptr), i++) {
    if (i == 0) { // domain
      domain = strdup(token);
    } else if (i == 1) { // port
      port = strdup(token);
    } else if (i == 2) {
      is404 = true;
    }
  }
  free(domain_port);
  if (port == NULL) port = strdup("80");

  if (domain == NULL || !vhost_exists(domain) || !config_value_exists(domain, "port")
      || atoi(config_get_value(domain, "port")) != aux->port || atoi(port) != aux->port) {
    is404 = true;
  }
  free(port);
  if (is404)
    push404(aux);

  http_put_val(http, HTTP_TRIMMED_DOMAIN, domain);
  free(domain);

  return is404;
}

/*
 * generic header builder for 416, 200, 206, 304 statuses
 */
static UT_string *build_header(int length, int full_length, const char *type, int status, char *hash_code, bool accept_range, int s, int e){
  UT_string *header;
  utstring_new(header);
  if(status == 416){
    utstring_printf(header, "HTTP/1.1 416 Range Not Satisfiable\r\n");
    utstring_printf(header, "Content-Range: bytes */%d\r\n\r\n", full_length);
    return header;
  }

  if (strstr(type, ".jpg"))
    type = "image/jpeg";
  else if (strstr(type, ".mp4"))
    type = "video/mp4";
  else
    type = "text/html";

  if(status == 200){
    utstring_printf(header, "HTTP/1.1 200 OK\r\n");
  }else if(status == 206) {
    utstring_printf(header, "HTTP/1.1 206 Partial Content\r\n");
  }else if(status == 304){
    utstring_printf(header, "HTTP/1.1 304 Not Modified\r\n");
  }else{
    //Its not a problem of user, so I'm not going to log it. Code is just wrong
    assert(0);
  }

  if(accept_range) length = e - s + 1;
  utstring_printf(header,  "Content-Length: %d\r\n", length);
  if(accept_range) {
    utstring_printf(header, "Accept-Ranges: bytes\r\n");
    utstring_printf(header, "Content-Range: bytes %d-%d/%d\r\n", s, e, full_length);
  }
  if(hash_code != 0){
    utstring_printf(header, "Cache-Control: max-age=5\r\n");
    utstring_printf(header, "ETag: %s\r\n", hash_code);
  }
  utstring_printf(header,  "Content-Type: %s\r\n\r\n", type);


  return header;
}

/*
 * constructs and pushes directory structure in html, for directories without index.html
 */
static void push_construct_dir(struct processor_state *aux, DIR *dir) {
  UT_string *header, *body;
  utstring_new(body);

  utstring_printf(body, "<html><title>I love gio</title><body>"
          "<h2>Directory listing</h2>"
          "<hr>"
          "<ul>");
  struct dirent *d;
  while (d = readdir(dir))
    if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) {
      if (d->d_type != DT_DIR)
        utstring_printf(body, "<li><a href=\"%s\">%s</a>\n", d->d_name, d->d_name);
      else
        utstring_printf(body, "<li><a href=\"%s/\">%s/</a>\n", d->d_name, d->d_name);
    }


  utstring_printf(body, "</ul>"
          "<hr>"
          "</body>"
          "</html>");

  char hash[30];
  etag_generate_str(hash, 20, utstring_body(body), strlen(utstring_body(body)));

  const char *last_hash;
  int status = 200;
  if(last_hash = http_get_val(aux->log_data.root, "if-none-match"))
    if(!strcmp(last_hash, hash)){
      status = 304;
      utstring_free(body);
      utstring_new(body);
  }
  header = build_header((int)strlen(utstring_body(body)), -1, ".html", status, hash, false, -1, -1);

  utstring_printf(header, "%s", utstring_body(body));

  int err = write(aux->fd, utstring_body(header), strlen(utstring_body(header)));
  if (err >= 0) aux->log_data.sent_length += err;
  aux->log_data.status_code = 200;

  if (err < 0 || (unsigned) err != strlen(utstring_body(header))) {
    char str[100];
    sprintf(str, "Was pushing directory content but could not push it fully, pushed only %d",
            aux->log_data.sent_length);
    log_write_error(&aux->log_data, str);
  }

  utstring_free(body);
  utstring_free(header);
}

/*
 * extension of send_file, supports cache and can also send segments
 */
static void send_file_gio(struct log_info *log, int fd, int file_fd, const char *type) {

  int full_length;
  int length = full_length = lseek(file_fd, 0, SEEK_END) - 1;
  lseek(file_fd, 0, SEEK_SET);

  const char *S = http_get_val(log->root, HTTP_SEND_S);
  const char *E = http_get_val(log->root, HTTP_SEND_E);
  int s,e;
  if(S == NULL) s = 0;
  else s = atoi(S);

  if(E == NULL) e = length - 1;
  else e = atoi(E);

  s = MAX(s, 0);
  e = MIN(e, length - 1);
  int status = (E || S) ? 206 : 200;

  char hash[30];
  etag_generate(hash, 20, file_fd);

  const char *last_hash;
  if(last_hash = http_get_val(log->root, "if-none-match"))
    if(!strcmp(last_hash, hash)){
      status = 304;
      length = 0;
    }

  if(s >= length || e < 0 || s > e){
    status = 416;
    length = 0;
  }

  UT_string *header = build_header(length, full_length, type, status, hash, true, s, e);
  int err = write(fd, utstring_body(header), strlen(utstring_body(header)));
  if (err >= 0) log->sent_length += strlen(utstring_body(header));
  log->status_code = status;

  if (err < 0 || (unsigned) err != strlen(utstring_body(header))) {
    char str[100];
    sprintf(str, "Could not send the file, sent only %d", err);
    log_write_error(log, str);
  }

  if(status != 304) {
    lseek(file_fd, 0, SEEK_SET);
    off_t of = s;

    err = sendfile(fd, file_fd, &of, e - s + 1);
    if (err >= 0)
      log->sent_length += err;
    if (err < 0 || err != e - s + 1) {
      char str[100];
      sprintf(str, "sendfile sent less %d", err);
      log_write_error(log, str);
    }
  }
  utstring_free(header);
}

/*
 * cgi case processor
 */
static void do_cgi(struct processor_state *aux, http_map_entry *http){
  int fd = cgi_bin_execute(http);
  if(fd >= 0) {
    char c[100];
    int red;
    int err = write(aux->fd,  "HTTP/1.1 200 OK\r\n", strlen( "HTTP/1.1 200 OK\r\n"));
    if(err > 0) aux->log_data.sent_length += err;
    aux->log_data.status_code = 200;
    while ((red = read(fd, c, 1000)) > 0) {
      int ret = write(aux->fd, c, red);
      if (ret != red) {
        fprintf(stderr, "\n\nCould not do somthing %d %d\n\n", ret, red);
      }
      aux->log_data.sent_length += red;
    }
    close(fd);
  }else{
    push404(aux);
  }
}

/*
 * main function for processing request
 */
void processor_inner_routine(struct processor_state *aux, http_map_entry *http) {
  if (was404_error(aux, http)) {
    return;
  }

  if(is_cgi_bin(http_get_val(http, HTTP_URI))){
    do_cgi(aux, http);
    return;
  }

  char *main_folder = config_get_value(http_get_val(http, HTTP_TRIMMED_DOMAIN), "documentroot");
  const char *relative_folder = http_get_val(http, HTTP_URI);
  if (strstr(relative_folder, "..")) {
    fprintf(stderr, "dots(..) are prohibited for safety reasons\n");
    push404(aux);
    return;
  }

  char *help_folder = strcat(calloc(strlen(relative_folder) + strlen(main_folder) + 20, 1), main_folder);
  help_folder = strcat(help_folder, "/");
  help_folder = strcat(help_folder, relative_folder);
  DIR *dir = opendir(help_folder);

  int file_fd = open(help_folder, O_RDONLY);
  help_folder = strcat(help_folder, "/index.html");
  int index_html_file_fd = open(help_folder, O_RDONLY);

  if (index_html_file_fd >= 0) {
    send_file_gio(&aux->log_data, aux->fd, index_html_file_fd, ".html");
  } else if (dir) {
    push_construct_dir(aux, dir);
  } else if (file_fd >= 0) {
    help_folder[strlen(help_folder) - strlen("/index.html")] = 0;
    int ind = strlen(help_folder) - 6;
    if (ind < 0) ind = 0;
    send_file_gio(&aux->log_data, aux->fd, file_fd, help_folder + ind);
  } else {
    log_write_error(&aux->log_data, "Could not chdir to relative folder");
    push404(aux);
  }

  close(file_fd);
  free(help_folder);
  closedir(dir);
  close(index_html_file_fd);
}

/*
 * When worker function receives ready socket, it executes this function.
 * Does abstraction on keep-alive for processor_inner_routine
 */
static long long processor_state_routine(struct processor_state *aux) {

  while (1) {
    http_map_entry *http = http_parse(aux->fd);
    if(http == NULL){
      fprintf(stderr, "Could not read something, fuck it anyway.\n");
      break;
    }

    http_map_entry *entry = NULL;
    HASH_FIND_STR(http, "connection", entry);
    bool keep_alive = (entry != NULL) && (strcmp(entry->value, "keep_alive") == 0);

    aux->log_data.root = http;
    aux->log_data.status_code = -1;
    aux->log_data.sent_length = 0;

    processor_inner_routine(aux, http);

    log_write_info(&aux->log_data);

    http_destroy(http);
    if (keep_alive) {
      struct timeval tv;

      tv.tv_sec = 5;
      tv.tv_usec = 0;

      setsockopt(aux->fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,sizeof(struct timeval));

      char c;
      if(recv(aux->fd, &c, 1, MSG_PEEK) <= 0)
        break;

    } else
      break;
  }

  close(aux->fd);
}

/*
 * every listener thread executes this function
 */
static void *one_port_listener(void *aux) {
  int port = (long) aux;


  int server_fd, err;
  struct sockaddr_in server, client;

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    fprintf(stderr, "could not create socket \n");
    exit(1);
  }

  server.sin_family = AF_INET;
  server.sin_port = htons(port);
  server.sin_addr.s_addr = htonl(INADDR_ANY);

  int opt_val = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof opt_val);

  err = bind(server_fd, (struct sockaddr *) &server, sizeof(server));
  if (err < 0) {
    fprintf(stderr, "error in binding \n");
    exit(1);
  }

  err = listen(server_fd, 128);

  if (err < 0) {
    fprintf(stderr, "Could not listen on socket\n");
    exit(1);
  }

  printf("Server is listening on %d\n", port);
  unsigned clilen = sizeof(client);
  while (1) {
    int newsockfd = accept(server_fd,
                           (struct sockaddr *) &client,
                           &clilen);
    if (newsockfd < 0)
      return NULL;

    processor_state *data = malloc(sizeof(processor_state));
    assert(data);

    data->fd = newsockfd;
    data->port = port;
    data->start_routine = processor_state_routine;
    data->log_data.ipAddr = client.sin_addr;
    data->log_data.root = NULL;
    data->log_data.sent_length = data->log_data.status_code = -1;
    processor_add(newsockfd, 1, data);
  }
}

/*
 * creates threads per port(listening)
 */
void start_server(config_map_entry *root) {
  processor_init();

  pthread_t *trd[1 << 16];
  memset(trd, 0, sizeof(trd));

  config_map_entry *item1, *tmp1;
  HASH_ITER(hh, root, item1, tmp1) {
    config_map_entry *e = NULL;
    HASH_FIND_STR(item1->sub, "port", e);
    if (e) {
      long port = atoi(e->value);
      if (trd[port]) continue;
      assert(trd[port] = malloc(sizeof(pthread_t)));
      assert(!pthread_create(trd[port], NULL, one_port_listener, (void *) port));
    }
  }

  for (int i = 0; i < (1 << 16); i++)
    if (trd[i])
      pthread_join(*trd[i], NULL), free(trd[i]);

}