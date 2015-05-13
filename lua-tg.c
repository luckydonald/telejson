/*
	This file is partly part of telegram-cli.

	Telegram-cli is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Telegram-cli is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this telegram-cli.  If not, see <http://www.gnu.org/licenses/>.

*/
/*
Compile on my Mac system: (else just copy this code into lua-tg.c and use make as usual)
gcc -I. -I. -g -O2  -I/usr/local/include -I/usr/include -I/usr/include   -DHAVE_CONFIG_H -Wall -Wextra -Werror -Wno-deprecated-declarations -fno-strict-aliasing -fno-omit-frame-pointer -ggdb -Wno-unused-parameter -fPIC -c -MP -MD -MF dep/lua-tg.d -MQ objs/lua-tg.o -o objs/lua-tg.o msg-server-tg.c
make
bin/telegram-cli -P 1337 -s 127.0.0.1:4458



One-liner:
echo -e "\n\n\n" && gcc -I. -I. -g -O2  -I/usr/local/include -I/usr/include -I/usr/include   -DHAVE_CONFIG_H -Wall -Wextra -Werror -Wno-deprecated-declarations -fno-strict-aliasing -fno-omit-frame-pointer -ggdb -Wno-unused-parameter -fPIC -c -MP -MD -MF dep/lua-tg.d -MQ objs/lua-tg.o -o objs/lua-tg.o msg-server-tg.c && make && cp bin/telegram-cli /Users/tasso/Library/Caches/clion10/cmake/generated/3b825333/3b825333/Debug1/tg
*/
//#include "lua_tg_version.h"

#ifndef PYTG2_CLI_VERSION
#define PYTG2_CLI_VERSION "tg.0.3"
//#define PYTG2_CLI_GIT_COMMIT ""
//#define PYTG2_CLI_GIT_BRANCH ""
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

//colors
#include "interface.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

//only one socket at a time.
#include "sema.h"
//close():
#include <unistd.h>

//exit():
#include <stdlib.h>
#include "tgl/tgl-layout.h"
#include "tgl/tgl.h"

// while debug:
//#include "tgl/tgl.h"
//#include "tgl/generate.h"

// va_list, va_start, va_arg, va_end
#include <stdarg.h>
#include <arpa/inet.h>
#include <assert.h>

#include <sys/errno.h>



// A macro to shorten the error output to just the string used in perror.
#define DIE(error_string) \
 int err = errno;\
 perror(error_string);\
 exit(err);

#define SOCKET_ANSWER_MAX_SIZE (1 << 25)
#define BLOCK_SIZE 256
#define PREFIX_LENGTH 16
static char socket_answer[SOCKET_ANSWER_MAX_SIZE + 1];
static int answer_pos = -1;
static int have_address = 0;

#define FRESHNESS_OLD -1
#define FRESHNESS_STARTUP 0
#define FRESHNESS_NEW 1

int msg_freshness = FRESHNESS_OLD; // -1: old (binlog), 0: startup (diff), 1: New

extern struct tgl_state *TLS;
extern int safe_quit;


//packet size
int socked_fd = -1;
struct sockaddr_in serv_addr;
int socked_in_use = 0;
rk_sema_t *edit_list;
rk_sema_t *edit_socket_status;

#define DEFAULT_PORT 4458

void socket_init (char *address_string);
int answer_started();
int answer_start();
void answer_end();
void socket_connect();
int socket_send();
void socket_close();

void lua_init(const char *address_string);
void lua_new_msg (struct tgl_message *M);
void lua_file_callback (struct tgl_state *TLSR, void *cb_extra, int success, char *file_name);

int valid_digit(char *ip_str);
int is_valid_ip(char *ip_str);
char* expand_escapes_alloc(const char* src);
char* malloc_formated(char const *format, ...);


void push_message (struct tgl_message *M);
void push_chat_info (struct tgl_chat *C);
static void push (const char *format, ...) __attribute__ ((format (printf, 1, 2)));
//#define push(...) answer_add_printf (__VA_ARGS__)
void print_no_address() {
	printf(
			COLOR_YELLOW "PYTG2: " COLOR_REDB "No given address to bind.\n"
			COLOR_RED    "Use `-s IP[:Port]`.\n" COLOR_NORMAL);
	return;
}

void lua_init (const char *address_string) {
	printf(COLOR_GREEN "\n"
			"==========================\n"
			" | Started PYTG2 plugin |\n"
			" | Version %12s |\n"
			"==========================\n\n" COLOR_NORMAL,
			PYTG2_CLI_VERSION
	);
	if (!address_string) {
		return print_no_address();
	}
	have_address = 1;
	char *address_string_copy = malloc(sizeof(char) * strnlen(address_string,23));
	strcpy(address_string_copy, address_string);
	socket_init(address_string_copy);
	edit_list = malloc(sizeof(struct rk_sema));
	edit_socket_status = malloc(sizeof(struct rk_sema));
	rk_sema_init(edit_list, 1); //can be lowered once.
	rk_sema_init(edit_socket_status, 1); //can be lowered once.
	//socket_connect();
	//socket_close();
}
union function_args {
	struct {
		int success; void *cb_extra; char *file_name;} file_download;
};
struct function {
	void (*callback)(union function_args *);
	union function_args *args;
	struct function *next;

};
struct function *get_last_function(struct function *func){
	assert(func);
	if (func->next == NULL) {
		return func;
	} else {
		return get_last_function(func->next);
	}
}
struct function *delayed_callbacks;

void append_function (struct function *func){
	rk_sema_wait(edit_list);
	if (delayed_callbacks == NULL) {
		delayed_callbacks = func;
	} else {
		get_last_function(delayed_callbacks)->next = func;
	}
	rk_sema_post(edit_list);
}
struct function *pop_function (){
	rk_sema_wait(edit_list);
	if (delayed_callbacks == NULL) {
		rk_sema_post(edit_list);
		return NULL;
	}
	struct function *tmp = delayed_callbacks;
	delayed_callbacks = delayed_callbacks->next;
	rk_sema_post(edit_list);
	return tmp;
}

void postpone(struct function *func) {
	append_function(func);
}
void postpone_execute_next() {
	struct function *func = pop_function();
	if(func != NULL)  // Queue is not empty.
	{
		void (*callback) (union function_args *) = func->callback;
		if (func->args)
		{
			union function_args *args = func->args;
			callback(args);
			free(func->args);
		}
		else
		{
			callback(NULL);
		}
	}
	free(func);
}

void push_freshness();

void answer_send();

void lua_new_msg (struct tgl_message *M)
{
	if (!have_address) { return print_no_address(); }
	assert(M);
	answer_start();
	printf("Generating Message...\n");
	push("{\"event\":\"message\", ");
	push_freshness();
	if (M->flags & FLAG_CREATED)
	{
		push(",");
		push_message (M);
	}
	push("}");
	answer_send();
}

void lua_new_chat_info (struct tgl_chat *C)
{
        // tgl_peer_t *U = (void *)C;
        
        answer_start();
        printf("New Chat info...\n");
        push("{\"event\":\"chat_info\",");
        push_chat_info (C);
        push("}");
      	answer_send();
}

void push_freshness()
{
	push("\"freshness\":\"");
	switch (msg_freshness) {
			case FRESHNESS_OLD:
				push("old\"");
			break;
		case FRESHNESS_STARTUP:
				push("startup\"");
			break;
		case FRESHNESS_NEW:
				push("new\"");
				break;
			default:
				printf("PYTG2: ERROR: Freshness (%i) is off the charts!", msg_freshness);
				assert (0 && "Hit default of freshness!");
		}
	// already closed the quotes.
}


void lua_file_callbackback(union function_args *arg);

//actually is not external/lua call, but the defined callback.
void lua_file_callback (struct tgl_state *TLSR, void *cb_extra, int success, char *file_name) {
	union function_args *arg = malloc(sizeof(union function_args));
	arg->file_download.success = success;
	arg->file_download.cb_extra = cb_extra;
	if(file_name) {
		char *file_name_persistent = malloc_formated("%s",file_name); // + null-terminator
		arg->file_download.file_name = file_name_persistent;
	} else { //is NULL
		arg->file_download.file_name = NULL;
	}
	if (!answer_started()){
		lua_file_callbackback(arg);
		free(arg);
	}
	else {
		struct function *new_function = malloc(sizeof(struct function));
		new_function->callback = lua_file_callbackback;
		new_function->args = arg;
		new_function->next = NULL;
		postpone(new_function);
	}
}
void lua_file_callbackback(union function_args *arg) {
	if (answer_started()){
		struct function *new_function = malloc(sizeof(struct function));
		new_function->callback = lua_file_callbackback;
		union function_args *args = malloc(sizeof(union function_args));
		memcpy(args, arg, sizeof(union function_args));
		new_function->args = args;
		new_function->next = NULL;
		postpone(new_function);
		return;
	}
	answer_start();
	char *file_name = arg->file_download.file_name;
	long long int *msg_id = arg->file_download.cb_extra;
	int success = arg->file_download.success;
	if (success) {
		push("{\"event\":\"download\", \"id\":%lld, \"file\":\"%s\"}", *msg_id, file_name); //TODO: msg number.
	} else {
		push("{\"event\":\"download\", \"id\":%lld, \"file\":null}", *msg_id);
	}
	free(file_name);
	free(msg_id);
	answer_send();
}


/* parse the address and set the serv_addr */
void socket_init (char *address_string)
{
	if (address_string == NULL)
	{
		printf(COLOR_REDB "No address and no port given.\n" COLOR_NORMAL);
		exit(3);
	}
	char *port_pos = NULL;
	uint16_t port = DEFAULT_PORT;
	strtok(address_string, ":");
	port_pos = strtok(NULL, ":"); //why is it needed doubled?
	int valid = is_valid_ip(address_string);
	if (!valid) {
		printf(COLOR_REDB "Invalid IP address given.\n" COLOR_NORMAL);
		exit(3);
	}
	if (port_pos == NULL)
	{
		printf(COLOR_YELLOW "Address: \"%s\", no port given, using port %i instead.\n" COLOR_NORMAL, address_string, port);
	}
	else
	{
		errno = 0;
		port = atoi(port_pos);
		if (errno != 0) {
			DIE("port number");
		}
		printf("Address: \"%s\", IP: %i.\n", address_string, port);
	}
	memset((void *) &serv_addr, 0, sizeof(struct sockaddr_in));
	serv_addr.sin_family = AF_INET; //still TCP
	inet_pton(AF_INET, address_string, &(serv_addr.sin_addr)); //copy the adress. //TODO: error validation?
	serv_addr.sin_port = (in_port_t) htons((uint16_t) port);
}

void socket_connect() {
	while(socked_fd == -1 && !safe_quit)
	{
		socked_fd = socket(AF_INET, SOCK_STREAM, 0); //lets do UDP Socket and listener_d is the Descriptor
		if (socked_fd == -1)
		{
			perror("socket");
			sleep (1);
			continue; //new try.
		}
		else
		{
			printf("Socket opened.\n");
		}
		int connection = connect(socked_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
		if (connection == -1)
		{
			perror("connect");
			socket_close();
			sleep (1);
			continue;
		}
	}
}

void answer_send()
{
	//rk_sema_wait(rk_se);
	int did_send = 0;
	do {
		socket_connect();
		if (safe_quit) {
			socket_close();
			break;
		}
		did_send = socket_send();
		socket_close();
	} while (did_send != 1 && !safe_quit);
	answer_end();
}

int answer_started() {
	return socked_in_use;
}
int answer_start() {
	rk_sema_wait(edit_socket_status);
	if(socked_in_use) {
		rk_sema_post(edit_socket_status);
		return 0;
	}
	assert(socked_in_use == 0);
	answer_pos = 0;
	socked_in_use = 1;
	rk_sema_post(edit_socket_status);
	return 1;
}
void answer_end() {
	rk_sema_wait(edit_socket_status);
	memset(socket_answer, 0, answer_pos); //reset da data.
	answer_pos = -1;
	assert(socked_in_use == 1);
	socked_in_use = 0;
	rk_sema_post(edit_socket_status);
}

int recv_all(int sockfd, void *buf, size_t len, int flags)
;


int get_acknowledge(const char *string);

int socket_send()
{
	/**
	 * returns 1, if successfully send (or there was nothing to send.) and the client responded with 'ACK'
	 * returns 0, if the client responded 'ERR'
	 * returns -1, if the client response timed out (10 seconds).
	 **/
	if (answer_pos > 0) {
		printf("Message length: %i\n", answer_pos);
		printf("Sending response: " COLOR_GREY "%.*s" COLOR_NORMAL "\n", answer_pos, socket_answer);
		ssize_t sent = 0;
		int start = 0;

		//Send Prefix block
		static char length_prefix[PREFIX_LENGTH + 1] = ""; //"LENGTH 33554432\n" = 6+space+8+newline = 16 characters, 17 with terminating NULL
		memset (&length_prefix, 0, PREFIX_LENGTH + 1);
		sprintf (length_prefix, "LENGTH %08d\n", answer_pos);
		while(start < PREFIX_LENGTH)
		{
			sent = send(socked_fd, (void *)(length_prefix + start), (size_t) PREFIX_LENGTH - start, 0);
			if(sent==-1){
				perror("send");
				break;
			}
			//printf("Send %li of %i, starting %i\n", sent, answer_pos, start);
			start += sent;
		}
		//Get Ack block
		int ack = get_acknowledge("Header");
		if (ack <= 0) {
			return ack;
		}
		//Send the actual message
		size_t size = BLOCK_SIZE;
		start = 0;
		if(answer_pos - start < (int)size) // less than a size block.
		{
			size = (size_t)(answer_pos - start); //what is left.
		}
		while( start < answer_pos)
		{
			//printf("send(%i, (void *)(%p  + %i)=%p, %li, 0)",socked_fd, &socket_answer, start, (void *)(socket_answer  + start),size);
			sent = send(socked_fd, (void *)(socket_answer  + start), size, 0);
			if(sent==-1){
				perror("send");
				break;
			}
			//printf("Send %li of %i, starting %i\n", sent, answer_pos, start);
			start += sent;
			if(answer_pos - start < (int)size) // less than a size block.
			{
				size = (size_t)(answer_pos - start); //what is left.
			}
			//printf("starting %i, going %li\n", start, size);
		}
		return get_acknowledge("Message");
	}
	return 1;
}

int get_acknowledge(const char *string)
{
	/**
	 * returns 1, if successfully send (or there was nothing to send.) and the client responded with 'ACK'
	 * returns 0, if the client responded 'ERR'
	 * returns -1, if the client response timed out (10 seconds), or closed the connection, or recv did fail otherwise.
	 **/
	struct timeval tv;
	tv.tv_sec = 10;  /* 10 Secs Timeout */
	tv.tv_usec = 0;  // Not init'ing this can cause strange errors
	char response[4] = "   "; //3 Characters + 1 Null-termiantor
	memset(response, 0, 4);
	setsockopt(socked_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(struct timeval));
	errno = 0; // reset it, to be sure.
	int read = recv_all(socked_fd, response, 3, 0);
	if (read < 0) {
		int err_no = errno; //cache it, in case something else uses this.
		if (err_no == EAGAIN) {
			printf(COLOR_REDB "Client has not acknowledged %s before the timeout.\n" COLOR_NORMAL, string);
			return -1;
		}
		printf(COLOR_REDB "Failed to receive Client's response for acknowledgeing %s.\n" COLOR_NORMAL, string);
		errno = err_no;
		perror("receive");
		return -1;
	}else if (read == 0) {
		printf(COLOR_REDB "Client closed the connection before acknowledgeing %s.\n" COLOR_NORMAL, string);
		return -1;
	}
	if (strncmp(response,"ACK",3) == 0) { //strncmp == 0 means zero difference.
		printf(COLOR_GREEN "Client acknowledged %s.\n" COLOR_NORMAL, string);
		return 1;
	} else if (strncmp(response,"ERR",3) == 0){
		printf(COLOR_REDB "Client reported error for %s.\n" COLOR_NORMAL, string);
		return 0;
	} else {
		printf(COLOR_REDB "Client has not acknowledged %s.\n" COLOR_NORMAL, string);
		return -1;
	}
}

void socket_close()
{
	if (socked_fd && socked_fd != -1)
	{
		close(socked_fd);
		socked_fd = -1;
		printf("Socket closed.\n");
	}else
	{
		printf("Socket was closed.\n");
	}
}

int recv_all(int sockfd, void *buf, size_t len, int flags)
{
	size_t to_read = len;
	char  *bufptr = (char*) buf;

	while (to_read > 0)
	{
		ssize_t rsz = recv(sockfd, bufptr, to_read, flags);
		if (rsz <= 0)
			return rsz;  /* Error or other end closed connection */

		to_read -= rsz;  /* Read less next time */
		bufptr += rsz;  /* Next buffer position to read into */
	}

	return len;
}

void push (const char *format, ...) {
	if (answer_pos < 0) { return; }
	va_list ap;
	va_start (ap, format);
	answer_pos += vsnprintf (socket_answer + answer_pos, SOCKET_ANSWER_MAX_SIZE - answer_pos, format, ap);
	va_end (ap);
	if (answer_pos >= SOCKET_ANSWER_MAX_SIZE) { answer_pos = -1; }
}




char *format_peer_type (int x) {
	/*
		one of this strings:
		"user", "group", "encr_chat"
		Please note, "group" is normaly called "chat"!
	*/
	switch (x) {
		case TGL_PEER_USER:
			return "user";
		case TGL_PEER_CHAT:
			return "group";
		case TGL_PEER_ENCR_CHAT:
			return "encr_chat";
		default:
			assert (0);
			return("Nope.avi");
	}
}
char *format_bool(int boolean) {
	return (boolean ? "true": "false");
}
char *format_string_or_null(char *str) {
	if (str == NULL) {
		return "";
	} else {
		return str;
	}
}

void push_peer (tgl_peer_id_t id);
void push_peer_cmd(tgl_peer_id_t id);

void push_action(struct tgl_message_action *action);

void push_typing(enum tgl_typing_status status);

void push_user (tgl_peer_t *P) {
	char *escaped_caption = expand_escapes_alloc(P->user.first_name);
	push("\"first_name\":\"%s\", ", escaped_caption);
	free(escaped_caption);
	escaped_caption = expand_escapes_alloc(P->user.last_name);
	push("\"last_name\":\"%s\", ", escaped_caption);
	free(escaped_caption);
	escaped_caption = expand_escapes_alloc(format_string_or_null(P->user.real_first_name));
	push("\"real_first_name\": \"%s\", ", escaped_caption);
	free(escaped_caption);
	escaped_caption = expand_escapes_alloc(format_string_or_null(P->user.real_last_name));
	push("\"real_last_name\": \"%s\", ",escaped_caption);
	free(escaped_caption);
	push("\"phone\":\"%s\"", format_string_or_null(P->user.phone));
}
void push_chat_info (struct tgl_chat *C) {
        assert (C->title);
	char *escaped_caption = expand_escapes_alloc(C->title);
	push("\"title\":\"%s\", \"members_num\":%i", escaped_caption, C->users_num);
	free(escaped_caption);
	if (C->user_list) {
		push(", \"members\": [");
		int i;
		for (i = 0; i < C->users_num; i++) {
			if (i != 0) {
				push(", ");
			}
			tgl_peer_id_t id = TGL_MK_USER (C->user_list[i].user_id);
			push_peer (id);
		}
		push("]");
	}
}
void push_chat (tgl_peer_t *P) {
	push_chat_info(&P->chat);
}

void push_encr_chat (tgl_peer_t *P) {
	push ("\"user\": ");
	push_peer (TGL_MK_USER (P->encr_chat.user_id));
}

void push_peer (tgl_peer_id_t id) {
	/*
	Will be { id: int, type: string, cmd: string }
	 */
	push("{");
	push("\"id\":%i, \"type\":\"%s\", \"cmd\": \"", tgl_get_peer_id (id), format_peer_type (tgl_get_peer_type (id)));
	push_peer_cmd(id);
	push("\", \"print_name\": ");
	//Note: opend quote for print_name's value!

	tgl_peer_t *P = tgl_peer_get(TLS, id);
	//P is defined -> did not return.
	if (P && (P->flags & FLAG_CREATED))
	{
		push("\"%s\", ", tgl_peer_get(TLS, id)->print_name); //print_name

		switch (tgl_get_peer_type(id))
		{
			case TGL_PEER_USER:
				push_user(tgl_peer_get(TLS, id));
				break;
			case TGL_PEER_CHAT:
				push_chat(tgl_peer_get(TLS, id));
				break;
			case TGL_PEER_ENCR_CHAT:
				push_encr_chat(tgl_peer_get(TLS, id));
				break;
			default:
				assert(0);
		}
		push(", \"flags\": %i",  P->flags);
	}else{
		push("null"); // print_name null, if peer unknown.
	}
	push("}");
}

void push_peer_cmd(tgl_peer_id_t id) {
	switch (tgl_get_peer_type (id)) {
		case TGL_PEER_USER:
			push("user#%d", tgl_get_peer_id (id));
			break;
		case TGL_PEER_CHAT:
			push("chat#%d", tgl_get_peer_id (id));
			break;
		case TGL_PEER_ENCR_CHAT:
			push("encr_chat#%d", tgl_get_peer_id (id));
			break;
		default:
			assert (0);
	}
}

void push_geo(struct tgl_geo geo) {
	push("\"longitude\": %f, \"latitude\": %f", geo.longitude, geo.latitude);
}
void push_size(int size){
	push("\"size\":\"");
	if (size < (1 << 10)) {
		push("%dB", size);
	} else if (size < (1 << 20)) {
		push("%dKiB", size >> 10);
	} else if (size < (1 << 30)) {
		push("%dMiB", size >> 20);
	} else {
		push("%dGiB", size >> 30);
	}
	push("\", \"bytes\":%d", size);
}


void push_media(struct tgl_message_media *M, long long int *msg_id)
{
	push("{");
	long long int *msg_id_copy;
	switch (M->type) {
		case tgl_message_media_photo:
			push("\"type\": \"photo\", \"encrypted\": false");
			if (M->photo.caption && strlen (M->photo.caption))
			{
				char *escaped_caption = expand_escapes_alloc(M->photo.caption);
				push (", \"caption\":\"%s\"", escaped_caption); //file name afterwards.
				free(escaped_caption);
			}
			msg_id_copy = malloc(sizeof(*msg_id));
			memcpy(msg_id_copy, msg_id, sizeof(*msg_id));
			tgl_do_load_photo (TLS, &M->photo, lua_file_callback, msg_id_copy);
			break;
		case tgl_message_media_photo_encr:
			push("\"type\": \"photo\", \"encrypted\": true");
			break;
			/*case tgl_message_media_video:
			  case tgl_message_media_video_encr:
				lua_newtable (luaState);
				lua_add_string_field ("type", "video");
				break;
			  case tgl_message_media_audio:
			  case tgl_message_media_audio_encr:
				lua_newtable (luaState);
				lua_add_string_field ("type", "audio");
				break;*/
		case tgl_message_media_document:
			push("\"type\": \"document\", \"encrypted\": false, \"document\":\"");
			if (M->document.flags & FLAG_DOCUMENT_IMAGE) {
				push("image");
			} else if (M->document.flags & FLAG_DOCUMENT_AUDIO) {
				push("audio");
			} else if (M->document.flags & FLAG_DOCUMENT_VIDEO) {
				push("video");
			} else if (M->document.flags & FLAG_DOCUMENT_STICKER) {
				push("sticker");
			} else {
				push("document");
			}
			push("\"");
			msg_id_copy = malloc(sizeof(*msg_id));
			memcpy(msg_id_copy, msg_id, sizeof(*msg_id));
			tgl_do_load_document (TLS, &M->document, lua_file_callback, msg_id_copy); // will download & insert file name.
			if (M->document.caption && strlen (M->document.caption)) {
				char *escaped_caption = expand_escapes_alloc(M->document.caption);
				push(", \"caption\":\"%s\"", escaped_caption);
				free(escaped_caption);
			}

			if (M->document.mime_type) {
				push(", \"mime\":\"%s\"", M->document.mime_type);
			}

			if (M->document.w && M->document.h) {
				push(", \"dimension\":{\"width\":%d,\"height\":%d}", M->document.w, M->document.h);
			}

			if (M->document.duration) {
				push(", \"duration\":%d", M->document.duration);
			}
			push(", ");
			push_size(M->document.size);
			break;
		case tgl_message_media_document_encr:
			push("\"type\": \"document\", \"encrypted\": true, \"document\":\"");
			if (M->encr_document.flags & FLAG_DOCUMENT_IMAGE) {
				push("image");
			} else if (M->encr_document.flags & FLAG_DOCUMENT_AUDIO) {
				push("audio");
			} else if (M->encr_document.flags & FLAG_DOCUMENT_VIDEO) {
				push("video");
			} else if (M->encr_document.flags & FLAG_DOCUMENT_STICKER) {
				push("sticker");
			} else {
				push("document");
			}
			push("\""); //end of document's value, next is file name.
			msg_id_copy = malloc(sizeof(*msg_id));
			memcpy(msg_id_copy, msg_id, sizeof(*msg_id));
			tgl_do_load_encr_document (TLS, &M->encr_document, lua_file_callback, msg_id_copy); // will download & insert file name.
			//TODO: wait until the callback pushed the filename.
			if (M->encr_document.caption && strlen (M->document.caption)) {
				char *escaped_caption = expand_escapes_alloc(M->document.caption);
				push(", \"caption\":\"%s\"", escaped_caption);
				free(escaped_caption);
			}

			if (M->encr_document.mime_type) {
				push(", \"mime\":\"%s\"", M->document.mime_type);
			}

			if (M->encr_document.w && M->document.h) {
				push(", \"dimension\":{\"width\":%d,\"height\":%d}", M->document.w, M->document.h);
			}

			if (M->encr_document.duration) {
				push(", \"duration\":%d", M->encr_document.duration);
			}
			push(", ");
			push_size(M->encr_document.size);
			break;
		case tgl_message_media_unsupported:
			push("\"type\": \"unsupported\"");
			break;
		case tgl_message_media_geo:
			push("\"type\": \"geo\", ");
			push_geo(M->geo);
			push(", \"google\":\"https://maps.google.com/?q=%.6lf,%.6lf\"", M->geo.latitude, M->geo.longitude);
			break;
		case tgl_message_media_contact:
			push("\"type\": \"contact\", \"phone\": \"%s\", \"first_name\": \"%s\", \"last_name\": \"%s\", \"user_id\": %i",  M->phone, M->first_name, M->last_name, M->user_id);
			break;
		default:
			push("\"type\": \"\?\?\?\", \"typeid\":\"%d\"", M->type); //escaped "???" to avoid Trigraph. (see http://stackoverflow.com/a/1234618 )
			break;
	}
	push("}");
}

void push_message (struct tgl_message *M) {
	if (!(M->flags & FLAG_CREATED)) {
		return;
	}
	push("\"id\":%lld, \"flags\": %i, \"forward\":", M->id, M->flags);
	if (tgl_get_peer_type (M->fwd_from_id)) {
		push("{\"sender\": ");
		push_peer (M->fwd_from_id);
		push(", \"date\": %i}", M->fwd_date);
	} else {
		push("null");
	}
	push(", \"sender\":");
	push_peer (M->from_id);
	push(", \"receiver\":");
	push_peer (M->to_id);
	if(!M->out && (tgl_get_peer_type(M->to_id) == TGL_PEER_CHAT || tgl_get_peer_type(M->to_id) == TGL_PEER_GEO_CHAT))
	{
		assert(tgl_get_peer_id(M->from_id) != TLS->our_id  && "Message should not be from ourself!");
		push(", \"peer\":");
		push_peer (M->to_id);
	} else if (!M->out && (tgl_get_peer_type(M->to_id) == TGL_PEER_USER || tgl_get_peer_type(M->to_id) == TGL_PEER_ENCR_CHAT)){
		// assert(tgl_get_peer_id(M->to_id) != TLS->our_id && "Message should not be from ourself!");
		push(", \"peer\":");
		push_peer (M->from_id);
	} else {
		// own message (or something missed)
		push(", \"peer\": null");
	}
	push(", \"own\": %s, \"unread\":%s, \"date\":%i, \"service\":%s", format_bool(M->out), format_bool(M->unread), M->date, format_bool(M->service) );
	if (!M->service) {
		if (M->message_len > 0 && M->message) {
			char *escaped_message = expand_escapes_alloc(M->message);
			push(", \"text\": \"%s\", \"media\": null", escaped_message); // http://stackoverflow.com/a/3767300
			free(escaped_message);
		}
		if (M->media.type && M->media.type != tgl_message_media_none) {
			push(", \"text\": null, \"media\":");
			push_media(&M->media, &M->id);
		}
	} else {
		push(", \"action\": ");
		push_action(&(M->action));
	}
	// is no dict => no "}".
}

void push_action(struct tgl_message_action *action)
{
	push ("{\"type\":%d, \"text\":", action->type);
	switch (action->type) {
		case tgl_message_action_none:
			push("\"none\"");
			break;
		case tgl_message_action_geo_chat_create:
			push ("\"geochat created\"");
			break;
		case tgl_message_action_geo_chat_checkin:
			push ("\"checking in geochat\"");
			break;
		case tgl_message_action_chat_create:
			push ("\"created chat\", \"title\":\"%s\",\"members_num\":%d", action->title, action->user_num);
			break;
		case tgl_message_action_chat_edit_title:
			push ("\"changed chat title\",\"title\":\"%s\"", action->new_title);
			break;
		case tgl_message_action_chat_edit_photo:
			push ("\"changed chat photo\"");
			break;
		case tgl_message_action_chat_delete_photo:
			push ("\"deleted chat photo\"");
			break;
		case tgl_message_action_chat_add_user:
			push ("\"added user to chat\",\"user\":");
			push_peer (tgl_set_peer_id (TGL_PEER_USER, action->user));
			push ("\n");
			break;
		case tgl_message_action_chat_delete_user:
			push ("\"deleted user from chat\", \"user\":");
			push_peer (tgl_set_peer_id (TGL_PEER_USER, action->user));
			break;
		case tgl_message_action_set_message_ttl:
			push ("\"set secure chat timeout\", \"seconds\":%d", action->ttl);
			break;
		case tgl_message_action_read_messages:
			push ("\"marked messages read\",\"count\":%d", action->read_cnt);
			break;
		case tgl_message_action_delete_messages:
			push ("\"messages deleted\",\"count\":%d", action->delete_cnt);
			break;
		case tgl_message_action_screenshot_messages:
			push ("\"messages screenshoted\",\"count\":%d", action->screenshot_cnt);
			break;
		case tgl_message_action_flush_history:
			push ("\"cleared history\"");
			break;
		case tgl_message_action_resend:
			push ("\"resend query\"");
			break;
		case tgl_message_action_notify_layer:
			push ("\"updated layer\",\"layer\":%d", action->layer);
			break;
		case tgl_message_action_typing:
			push ("\"typing\",\"status\":%d,\"text\":\"", action->typing);
			push_typing (action->typing);
			push ("\"");
			break;
		case tgl_message_action_noop:
			push ("\"noop\"");
			break;
		case tgl_message_action_request_key:
			push ("\"request rekey\", \"id\":\"%016llx\"", action->exchange_id);
			break;
		case tgl_message_action_accept_key:
			push ("\"accept rekey\", \"id\":\"%016llx\"", action->exchange_id);
			break;
		case tgl_message_action_commit_key:
			push ("\"commit rekey\", \"id\":\"%016llx\"", action->exchange_id);
			break;
		case tgl_message_action_abort_key:
			push ("\"abort rekey\", \"id\":\"%016llx\"", action->exchange_id);
			break;
	}
	push("}");
}

void push_typing(enum tgl_typing_status status)
{
	switch (status) {
			case tgl_typing_none:
				push ("doing nothing");
				break;
			case tgl_typing_typing:
				push ("typing");
				break;
			case tgl_typing_cancel:
				push ("deleting typed message");
				break;
			case tgl_typing_record_video:
				push ("recording video");
				break;
			case tgl_typing_upload_video:
				push ("uploading video");
				break;
			case tgl_typing_record_audio:
				push ("recording audio");
				break;
			case tgl_typing_upload_audio:
				push ("uploading audio");
				break;
			case tgl_typing_upload_photo:
				push ("uploading photo");
				break;
			case tgl_typing_upload_document:
				push ("uploading document");
				break;
			case tgl_typing_geo:
				push ("choosing location");
				break;
			case tgl_typing_choose_contact:
				push ("choosing contact");
				break;
		}
}

void push_client_id (struct tgl_message *M)
{
	push("client_id: %i",TLS->our_id);
}

// http://stackoverflow.com/a/3535143
void expand_escapes(char* dest, const char* src)
{
	char c;

	while ((c = *(src++))) {
		switch(c) {
			case '\a':
				*(dest++) = '\\';
				*(dest++) = 'a';
				break;
			case '\b':
				*(dest++) = '\\';
				*(dest++) = 'b';
				break;
			case '\t':
				*(dest++) = '\\';
				*(dest++) = 't';
				break;
			case '\n':
				*(dest++) = '\\';
				*(dest++) = 'n';
				break;
			case '\v':
				*(dest++) = '\\';
				*(dest++) = 'v';
				break;
			case '\f':
				*(dest++) = '\\';
				*(dest++) = 'f';
				break;
			case '\r':
				*(dest++) = '\\';
				*(dest++) = 'r';
				break;
			case '\\':
				*(dest++) = '\\';
				*(dest++) = '\\';
				break;
			case '\"':
				*(dest++) = '\\';
				*(dest++) = '\"';
				break;
			/*case '\'':
				*(dest++) = '\\';
				*(dest++) = '\'';
				break;*/
			default:
				*(dest++) = c;
		}
	}

	*dest = '\0'; /* Ensure nul terminator */
}
/* Returned buffer may be up to twice as large as necessary */
char* expand_escapes_alloc(const char* src)
{
	char* dest = malloc(2 * strlen(src) + 1);
	expand_escapes(dest, src);
	return dest;
}
// format (like printf) to a newly malloc'ed string.
char* malloc_formated(char const *format, ...) {
	va_list arglist, arglist_copy;
	va_start( arglist, format);
	size_t needed = vsnprintf(NULL, 0, format, arglist) + 1; //plus 1 for terminating NULL
	va_end(arglist);
	char  *buffer = malloc(needed);
	va_start( arglist_copy, format);
	vsnprintf(buffer, needed, format, arglist_copy);
	va_end( arglist );
	*(buffer + needed - 1) = '\0'; // ensure nul terminator
	return buffer;
}



// Empty stuff:
void lua_secret_chat_update (struct tgl_secret_chat *C, unsigned flags) {
	if (!have_address) { return; }
	return;
}
void lua_user_update (struct tgl_user *U, unsigned flags) {
	if (!have_address) { return; }
	printf("User Update (#%i \"%s\").\n", U->id.id, U->first_name);
	return;
}
void lua_diff_end (void) {
	/* Did all message since last session. */
	if (!have_address) { return print_no_address(); }
	msg_freshness = FRESHNESS_NEW;
	return;
}
void lua_do_all (void) {
	if (!have_address) { return; }
	postpone_execute_next();
	return;
}
void lua_our_id (int id) {
	if (!have_address) { return print_no_address(); }
	printf("lua_our_id: %i\n", id);
	return;
}
void lua_binlog_end (void) {
	/* Did all old messages.
	next comes new message (since last session -> lua_diff_end) */
	if (!have_address) { return; }
	msg_freshness = FRESHNESS_STARTUP;
	return;
}
void lua_chat_update (struct tgl_chat *C, unsigned flags) {
	if (!have_address) { return; }
	return;
}


//www.geeksforgeeks.org/program-to-validate-an-ip-address/
/* return 1 if string contain only digits, else return 0 */
int valid_digit(char *ip_str)
{
	while (*ip_str) {
		if (*ip_str >= '0' && *ip_str <= '9')
			++ip_str;
		else
			return 0;
	}
	return 1;
}

#define IP_DELIM "."

//www.geeksforgeeks.org/program-to-validate-an-ip-address/
/* return 1 if IP string is valid, else return 0 */
int is_valid_ip(char *ip_str)
{
	if (ip_str == NULL) {
		return 0;
	}
	int num, dots = 0;
	char *ptr;

	char *ip_str_copy = malloc(sizeof(char) * strnlen(ip_str, 16)); //copy because we modify the string. 123.567.901.345\0 = 15 + \0 = 16
	strncpy(ip_str_copy, ip_str, 16);
	ptr = ip_str_copy + 16;
	*ptr = '\0'; //force null-termination.


	if (ip_str_copy == NULL) {
		free(ip_str_copy);
		return 0;
	}
	// See following link for strtok()
	// http://pubs.opengroup.org/onlinepubs/009695399/functions/strtok_r.html
	ptr = strtok(ip_str_copy, IP_DELIM);

	if (ptr == NULL) {
		free(ip_str_copy);
		return 0;
	}

	while (ptr)
	{

		/* after parsing string, it must contain only digits */
		if (!valid_digit(ptr)) {
			free(ip_str_copy);
			return 0;
		}

		num = atoi(ptr);

		/* check for valid IP */
		if (num >= 0 && num <= 255) {
			/* parse remaining string */
			ptr = strtok(NULL, IP_DELIM);
			if (ptr != NULL) {
				++dots;
			}
		} else {
			free(ip_str_copy);
			return 0;
		}
	}
	/* valid IP string must contain 3 dots */
	if (dots != 3) {
		free(ip_str_copy);
		return 0;
	}
	free(ip_str_copy);
	return 1;
}
