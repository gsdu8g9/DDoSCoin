
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/pem.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>

#include <assert.h>
#include "tls.h"

#define TIMEOUT_SECS 3

struct config {
    struct event_base *base;

    uint8_t     prev_block_hash[32];
    uint8_t     merkle_root[32];

    size_t      num_connections;

    // TODO: remove
    RSA         *public_key;
    struct sockaddr_in  sin;
    FILE        *rand;
};

enum tls_state {
    INVALID=-1, WAIT_HELLO, WAIT_CERT, WAIT_KEYX, KEYX_RECV,
};

struct conn_state {
    struct config       *conf;
    evutil_socket_t     sock;
    struct bufferevent  *bev;

    enum tls_state      state;

    uint8_t             nonce[32];
    uint8_t             client_random[32];

    uint8_t             *shello_raw;
    uint8_t             *cert_raw;
    uint8_t             *keyx_raw;

    struct server_hello shello;
    struct server_keyx  skeyx;
};

void cleanup(struct conn_state *st);
void new_connection(struct config *conf);
void print_hex(char *prefix, uint8_t *d, size_t len);

void generate_nonce(struct config *conf, uint8_t *nonce)
{
    fread(nonce, 32, 1, conf->rand);
}

// 32-byte prev_block_hash (SHA256(prev_block))
// 32-byte merkle_root
// 32-byte nonce
// TODO: precompute hash over first two, only append hash of nonce (length-extend)
// for faster computation!
void generate_client_random(uint8_t *prev_block_hash, uint8_t *merkle_root,
                            uint8_t *nonce, uint8_t *client_random)
{

    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, prev_block_hash, 32);
    SHA256_Update(&ctx, merkle_root, 32);
    SHA256_Update(&ctx, nonce, 32);
    SHA256_Final(client_random, &ctx);
}

void handle_hello(struct conn_state *st)
{
    struct evbuffer *input = bufferevent_get_input(st->bev);
    size_t server_hello_len = get_tls_record(input, &st->shello_raw);

    if (server_hello_len == 0) {
        return;
    }

    st->state = WAIT_CERT;

    if (parse_server_hello(st->shello_raw, server_hello_len, &st->shello) < 0) {
        printf("bad server hello\n");
    }

}

void handle_cert(struct conn_state *st)
{
    struct evbuffer *input = bufferevent_get_input(st->bev);
    size_t cert_len = get_tls_record(input, &st->cert_raw);

    if (cert_len == 0) {
        return;
    }

    st->state = WAIT_KEYX;

    // TODO: parse cert
}

int satisfies_proof_of_work(struct conn_state *st)
{
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, st->skeyx.server_dh_params, st->skeyx.server_dh_params_len);
    SHA256_Update(&ctx, st->skeyx.sig, st->skeyx.sig_len);
    SHA256_Update(&ctx, st->nonce, 32);
    SHA256_Final(digest, &ctx);

    print_hex("   difficulty: ", digest, SHA256_DIGEST_LENGTH);
    if (digest[0] == 0x00) {
        return 1;
    }
    
    return 0;
}

void print_hex(char *prefix, uint8_t *d, size_t len)
{
    int i;
    printf("%s", prefix);
    for (i=0; i<len; i++) {
        printf("%02x", d[i]);
    }
    printf("\n");
}

void handle_keyx(struct conn_state *st)
{
    struct evbuffer *input = bufferevent_get_input(st->bev);
    size_t keyx_len = get_tls_record(input, &st->keyx_raw);

    if (keyx_len == 0) {
        return;
    }

    st->state = KEYX_RECV;

    if (parse_server_keyex(st->keyx_raw, keyx_len, st->shello.cipher_suite, &st->skeyx) < 0) {
        printf("bad keyx\n");
        return;
    }

    // Check if it passes the test
    if (satisfies_proof_of_work(st)) {
        printf("Winner!\n");
        print_hex("nonce: ", st->nonce, 32);
        print_hex("server_random: ", st->shello.random, 32);
        print_hex("server_dh_params: ", st->skeyx.server_dh_params, st->skeyx.server_dh_params_len);
        print_hex("sig: ", st->skeyx.sig, st->skeyx.sig_len);
    }
    cleanup(st);
}

void cleanup(struct conn_state *st)
{
    struct config *conf = st->conf;
    printf("cleanup called (%lu)\n", conf->num_connections);
    if (st->bev) {
        bufferevent_free(st->bev);
    }

    if (st->shello_raw) {
        free(st->shello_raw);
    }

    if (st->cert_raw) {
        free(st->cert_raw);
    }

    if (st->keyx_raw) {
        free(st->keyx_raw);
    }

    // TODO if extensions were populated in shello, clean those up too (we don't, though)

    free(st);

    new_connection(conf);
}

void readcb(struct bufferevent *bev, void *ptr)
{
    struct conn_state *st = ptr;

    if (st->state == WAIT_HELLO) {
        handle_hello(st);
    }
    if (st->state == WAIT_CERT) {
        handle_cert(st);
    }
    if (st->state == WAIT_KEYX) {
        handle_keyx(st);
    }
}

void eventcb(struct bufferevent *bev, short events, void *ptr)
{

    if (events & BEV_EVENT_CONNECTED) {
        // Yay?
    } else if (events & (BEV_EVENT_EOF | BEV_EVENT_TIMEOUT)) {
        // closed, cleanup
    }

}

void new_connection(struct config *conf)
{
    struct conn_state *st = calloc(1, sizeof(struct conn_state));
    st->state = WAIT_HELLO;
    st->conf = conf;

    st->sock = socket(AF_INET, SOCK_STREAM, 0);
    int optval = 1;
    setsockopt(st->sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    evutil_make_socket_nonblocking(st->sock);
    struct bufferevent *bev;
    bev = bufferevent_socket_new(conf->base, st->sock, BEV_OPT_CLOSE_ON_FREE);
    // TODO: error check
    struct timeval read_to;
    read_to.tv_sec = TIMEOUT_SECS;
    read_to.tv_usec = 0;
    bufferevent_set_timeouts(bev, &read_to, &read_to);
    st->bev = bev;

    // Generate client random
    generate_nonce(conf, st->nonce);
    generate_client_random(conf->prev_block_hash, conf->merkle_root,
                           st->nonce, st->client_random);

    // Generte client hello and send it
    uint8_t *client_hello;
    size_t client_hello_len = make_client_hello(st->client_random, &client_hello);

    evbuffer_add(bufferevent_get_output(bev), client_hello, client_hello_len);
    free(client_hello);

    bufferevent_setcb(bev, readcb, NULL, eventcb, st);
    // TODO: error check
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    if (bufferevent_socket_connect(bev, (struct sockaddr *)&conf->sin,
                                   sizeof(conf->sin)) < 0) {

        printf("socket connect failed :(\n");
    }

    conf->num_connections++;
}


// Initialize state and create #conns connections
void fetcher(struct config *conf, int conns)
{
    int i;
    for (i=0; i<conns; i++) {
        new_connection(conf);
    }
}

int main()
{
    struct config conf;
    memset(&conf, 0, sizeof(conf));

    // Lookup IP of target
    struct hostent *he = gethostbyname("ericw.us");
    memset(&conf.sin, 0, sizeof(conf.sin));
    conf.sin.sin_family  = he->h_addrtype;
    conf.sin.sin_port    = htons(443);
    conf.sin.sin_addr    = *(((struct in_addr **)he->h_addr_list)[0]);

    // Dummy values
    memset(conf.prev_block_hash, 0xAA, 32);
    memset(conf.merkle_root, 0xBB, 32);

    // Load RSA key
    FILE *fp = fopen("./ericw.us.pub", "rb");
    // Apparently, PEM_read_PUBKEY() doens't read PEM(?!?)
    PEM_read_RSA_PUBKEY(fp, &conf.public_key, NULL, NULL);
    fclose(fp);
    if (conf.public_key == NULL) {
        printf("Error couldn't read public key\n");
        return -1;
    }

    // Open /dev/urandom
    conf.rand = fopen("/dev/urandom", "rb");

    conf.base = event_base_new();

    fetcher(&conf, 1);

    event_base_dispatch(conf.base);



    return 0;
}