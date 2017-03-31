#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <trest.h>
#include <thttp.h>

#define MODULE_NAME             "pantahub-api"
#define sc_log(level, msg, ...)         vlog(MODULE_NAME, level, msg, ## __VA_ARGS__)
#include "log.h"

#include "systemc.h"
#include "utils.h"

#include "pantahub.h"

#define DEVICE_REGISTER_FMT "{ \"secret\" : \"%s\" }"
#define ENDPOINT_FMT "/devices/%s"

trest_ptr *client = 0;
char *endpoint;

static int connect_try(char *host, int port, int h_length)
{
	int ret = 0;
	int sfd = -1;
	struct sockaddr_in *serv = malloc(sizeof (struct sockaddr_in));
	char *addr;

	sfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

	memcpy((void *) &serv->sin_addr, host, h_length);
	serv->sin_family = AF_INET;
	serv->sin_port = htons(port);

	addr = malloc(h_length + 1);
	strncpy(addr, host, h_length);
	addr[h_length] = '\0';

	if ((ret = connect(sfd, (struct sockaddr *) serv, sizeof (*serv))) < 0)
		goto out;

	// succcess
	ret = 1;

out:
	if (sfd > -1)
		close(sfd);

	return ret;
}

static int ph_client_init(struct systemc *sc)
{
	int size;
        trest_auth_status_enum status = TREST_AUTH_STATUS_NOTAUTH;

	if (client)
		goto auth;

	client = trest_new_tls_from_userpass(
			sc->config->creds.host,
			sc->config->creds.port,
			sc->config->creds.prn,
			sc->config->creds.secret,
			sc_ph_get_certs(sc)
			);

auth:
	status = trest_update_auth(client);
	if (status != TREST_AUTH_STATUS_OK) {
		sc_log(DEBUG, "unable to auth unclaimed device, status=%d", status);
		return 0;
	}

	size = sizeof(ENDPOINT_FMT) + strlen(sc->config->creds.id) + 1;
	endpoint = malloc(size * sizeof(char));

	sprintf(endpoint, ENDPOINT_FMT, sc->config->creds.id);

	return 1;					
}

/* API */

const char** sc_ph_get_certs(struct systemc *sc)
{
	struct dirent **files;
	char **cafiles;
	char *dir = "/certs/";
	char path[128];
	int n = 0, i = 0, size = 0;

	n = scandir(dir, &files, NULL, alphasort);
	if (n < 0)
		return NULL;

	// Always n-1 due to . and .., and need one extra
	cafiles = calloc(1, (sizeof(char*) * (n-1)));

	while (n--) {
		if (!strncmp(files[n]->d_name, ".", 1))
			continue;

		sprintf(path, "/certs/%s", files[n]->d_name);
		size = strlen(path);
		cafiles[i] = malloc((size+1) * sizeof(char));
		strncpy(cafiles[i], path, size);
		cafiles[i][size] = '\0';
		i++;
	}

	while (i--) {
	}

	return (const char **) cafiles;
}

int sc_ph_is_available(struct systemc *sc)
{
	int ret = 0;
	int port = 0;
	struct hostent *ent;
	char *host;

	// default to global PH instance
	if (strcmp(sc->config->creds.host, "") == 0)
		host = "api.pantahub.com";
	else
		host = sc->config->creds.host;

	port = sc->config->creds.port;
	if (!port)
		port = 80;

	if ((ent = gethostbyname(host)) == NULL) {
		sc_log(DEBUG, "gethostbyname failed for %s", host);
		goto out;
	}

	// attempt to connect to host
	ret = connect_try(ent->h_addr_list[0], port, ent->h_length);

out:
	if (ret > 0) {
		sc_log(INFO, "PH available at '%s:%d'", host, port);
	} else {
		ret = 0;
	}

	return ret;
}

void sc_ph_release_client(struct systemc *sc)
{
	if (client) {
		trest_free(client);
		client = 0;
	}

	if (endpoint) {
		free(endpoint);
		endpoint = 0;
	}
}

int sc_ph_device_exists(struct systemc *sc)
{
	int ret = 0;
	char *id = 0;
	trest_request_ptr req = 0;
	trest_response_ptr res = 0;

	if (!ph_client_init(sc)) {
		sc_log(DEBUG, "failed to initialize PantaHub connection");
		goto out;
	}

	req = trest_make_request(TREST_METHOD_GET,
				 endpoint,
				 0, 0, 0);

	res = trest_do_json_request(client, req);

	if (!res->body)
		goto out;

	id = get_json_key_value(res->body, "id",
			res->json_tokv, res->json_tokc);

	if (id && (strcmp(id, "") != 0)) {
		sc_log(DEBUG, "device exists: '%s'", id);
		ret = 1;
	}

out:
	if (id)
		free(id);
	if (req)
		trest_request_free(req);
	if (res)
		trest_response_free(res);

	return ret;
}

int sc_ph_register_self(struct systemc *sc)
{
	int ret = 1;
	int tokc;
	char json[512];
	char *secret;
	thttp_request_tls_t* tls_req = 0;
	thttp_response_t* res = 0;
	jsmntok_t *tokv;

	tls_req = thttp_request_tls_new_0();
	tls_req->crtfiles = (char **) sc_ph_get_certs(sc);

	thttp_request_t* req = (thttp_request_t*) tls_req;

	req->method = THTTP_METHOD_POST;
	req->proto = THTTP_PROTO_HTTP;	
	req->proto_version = THTTP_PROTO_VERSION_10;

	req->host = sc->config->creds.host;
	req->port = sc->config->creds.port;

	req->path = "/devices/";

	secret = rand_string(10);
	sprintf(json, DEVICE_REGISTER_FMT, secret);
	req->body = json;

	req->headers = 0;
	req->body_content_type = "application/json";

	res = thttp_request_do(req);

	// If registered, override in-memory PantaHub credentials
	if (res->body) {
		jsmnutil_parse_json(res->body, &tokv, &tokc);
		sc->config->creds.id = get_json_key_value(res->body, "id",
							tokv, tokc);
		sc->config->creds.prn = get_json_key_value(res->body, "prn",
							tokv, tokc);
		sc->config->creds.secret = get_json_key_value(res->body, "secret",
							tokv, tokc);
	} else {
		sc_log(ERROR, "registration attempt failed");
		ret = 0;
	}

	if (secret)
		free(secret);
	if (req)
		thttp_request_free(req);
	if (res)
		thttp_response_free(res);	

	return ret;
}

int sc_ph_device_is_owned(struct systemc *sc, char **c)
{
	int ret = 1;
	char *owner = 0, *challenge = 0;
	trest_request_ptr req = 0;
	trest_response_ptr res = 0;

	if (!ph_client_init(sc)) {
		sc_log(ERROR, "failed to initialize PantaHub connection");
		ret = 0;
		goto out;
	}

	req = trest_make_request(TREST_METHOD_GET,
				 endpoint,
				 0, 0, 0);

	res = trest_do_json_request(client, req);

	owner = get_json_key_value(res->body, "owner",
			res->json_tokv, res->json_tokc);

	if (owner && (strcmp(owner, "") != 0)) {
		sc_log(DEBUG, "device-owner: '%s'", owner); 
		goto out;
	}

	challenge = get_json_key_value(res->body, "challenge",
			res->json_tokv, res->json_tokc);

	strcpy(*c, challenge);
	ret = 0;

out:
	if (owner)
		free(owner);
	if (challenge)
		free(challenge);
	if (req)
		trest_request_free(req);
	if (res)
		trest_response_free(res);

	return ret;
}

void sc_ph_update_hint_file(struct systemc *sc, char *c)
{
	int fd;
	char buf[256];

	fd = open("/tmp/pantavisor/device-id", O_TRUNC | O_SYNC | O_RDWR);
	if (!fd) {
		sc_log(INFO, "unable to open device-id hint file");
		return;
	}
	sprintf(buf, "device-id=%s\n", sc->config->creds.id);
	write(fd, buf, strlen(buf));
	close(fd);

	fd = open("/tmp/pantavisor/challenge", O_TRUNC | O_SYNC | O_RDWR);
	if (!fd) {
		sc_log(INFO, "unable to open challenge hint file");
		return;
	}
	sprintf(buf, "challenge=%s\n", c);
	write(fd, buf, strlen(buf));
	close(fd);		
}